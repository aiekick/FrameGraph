// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "VFrameGraph.h"
#include "VCommandBuffer.h"
#include "VSubmitted.h"
#include "Shared/PipelineResourcesHelper.h"
#include "stl/Algorithms/StringUtils.h"

namespace FG
{
	using CmdBatches_t			= VSubmitted::Batches_t;
	using SubmitInfos_t			= StaticArray< VkSubmitInfo, VSubmitted::MaxBatches >;
	using TempSemaphores_t		= VSubmitted::Semaphores_t;
	using PendingSwapchains_t	= FixedArray< VSwapchain const*, 16 >;
	using TempFences_t			= FixedArray< VkFence, 32 >;

/*
=================================================
	constructor
=================================================
*/
	VFrameGraph::VFrameGraph (const VulkanDeviceInfo &vdi) :
		_state{ EState::Initial },	_device{ vdi },
		_queueUsage{ Default },		_resourceMngr{ _device }
	{
	}
	
/*
=================================================
	destructor
=================================================
*/
	VFrameGraph::~VFrameGraph ()
	{
		CHECK( _GetState() == EState::Destroyed );
	}
	
/*
=================================================
	Initialize
=================================================
*/
	bool  VFrameGraph::Initialize ()
	{
		CHECK_ERR( _SetState( EState::Initial, EState::Initialization ));

		// setup queues
		{
			_AddGraphicsQueue();
			_AddAsyncComputeQueue();
			_AddAsyncTransferQueue();
			CHECK_ERR( not _queueMap.empty() );
		}

		CHECK_ERR( _resourceMngr.Initialize() );
		
		CHECK_ERR( _SetState( EState::Initialization, EState::Idle ));
		return true;
	}
	
/*
=================================================
	Deinitialize
=================================================
*/
	void  VFrameGraph::Deinitialize ()
	{
		CHECK_ERR( _SetState( EState::Idle, EState::Destroyed ), void());
		CHECK_ERR( WaitIdle(), void());

		// delete command buffers
		{
			EXLOCK( _cmdBuffersGuard );

			DEBUG_ONLY(
				FG_LOGI( "Max command buffers "s << ToString(_cmdBufferPool.CreatedObjectsCount()) );
				FG_LOGI( "Max command batches "s << ToString(_cmdBatchPool.CreatedObjectsCount()) );
				FG_LOGI( "Max submitted batches "s << ToString(_submittedPool.CreatedObjectsCount()) );
			)

			_cmdBufferPool.Release();
			_cmdBatchPool.Release();
			_submittedPool.Release();
		}

		// delete per queue data
		{
			for (auto& q : _queueMap)
			{
				CHECK( q.pending.empty() );
				CHECK( q.submitted.empty() );

				EXLOCK( q.cmdPoolGuard );
				q.cmdPool.Destroy( _device );

				for (auto& sem : q.semaphores) {
					_semaphoreCache.push_back( sem );
					sem = VK_NULL_HANDLE;
				}
			}
		}

		// delete fences
		{
			for (auto& fence : _fenceCache) {
				_device.vkDestroyFence( _device.GetVkDevice(), fence, null );
			}
			_fenceCache.clear();
		}

		// delete semaphores
		{
			for (auto& sem : _semaphoreCache) {
				_device.vkDestroySemaphore( _device.GetVkDevice(), sem, null );
			}
			_semaphoreCache.clear();
		}

		_shaderDebugCallback = {};
		_resourceMngr.Deinitialize();
	}
	
/*
=================================================
	AddPipelineCompiler
=================================================
*/
	bool  VFrameGraph::AddPipelineCompiler (const PipelineCompiler &comp)
	{
		CHECK_ERR( _IsInitialized() );

		_resourceMngr.AddCompiler( comp );
		return true;
	}
	
/*
=================================================
	SetShaderDebugCallback
=================================================
*/
	bool  VFrameGraph::SetShaderDebugCallback (ShaderDebugCallback_t &&cb)
	{
		CHECK_ERR( _IsInitialized() );

		_shaderDebugCallback = std::move(cb);
		return true;
	}
	
/*
=================================================
	GetDeviceInfo
=================================================
*/
	VFrameGraph::DeviceInfo_t  VFrameGraph::GetDeviceInfo () const
	{
		CHECK_ERR( _IsInitialized() );

		VulkanDeviceInfo	result;
		result.instance			= BitCast<InstanceVk_t>( _device.GetVkInstance() );
		result.physicalDevice	= BitCast<PhysicalDeviceVk_t>( _device.GetVkPhysicalDevice() );
		result.device			= BitCast<DeviceVk_t>( _device.GetVkDevice() );

		for (auto& src : _device.GetVkQueues())
		{
			auto&	dst = result.queues.emplace_back();
			dst.id			= BitCast<QueueVk_t>( src.handle );
			dst.familyIndex	= uint(src.familyIndex);
			dst.familyFlags	= BitCast<QueueFlagsVk_t>( src.familyFlags );
			dst.priority	= src.priority;
			dst.debugName	= src.debugName;
		}

		return result;
	}

/*
=================================================
	_TransitImageLayoutToDefault
=================================================
*/
	void  VFrameGraph::_TransitImageLayoutToDefault (RawImageID imageId, VkImageLayout initialLayout, uint queueFamily)
	{
		const auto&	image = *_resourceMngr.GetResource( imageId );
		
		if ( image.DefaultLayout() == initialLayout )
			return;

		VkImageMemoryBarrier	barrier = {};
		barrier.sType				= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask		= 0;
		barrier.dstAccessMask		= 0;
		barrier.oldLayout			= initialLayout;
		barrier.newLayout			= image.DefaultLayout();
		barrier.image				= image.Handle();
		barrier.subresourceRange	= { image.AspectMask(), 0, image.MipmapLevels(), 0, image.ArrayLayers() };

		// error will be generated by validation layer if current queue family
		// doesn't match with queue family in the command buffer
		barrier.srcQueueFamilyIndex	= queueFamily;
		barrier.dstQueueFamilyIndex	= queueFamily;

		for (auto& q : _queueMap)
		{
			if ( q.ptr and (uint(q.ptr->familyIndex) == queueFamily or queueFamily == VK_QUEUE_FAMILY_IGNORED) )
			{
				q.imageBarriers.push_back( barrier );
				return;
			}
		}

		ASSERT( !"can't find suitable queue" );
	}

/*
=================================================
	CreatePipeline
=================================================
*/
	MPipelineID  VFrameGraph::CreatePipeline (INOUT MeshPipelineDesc &desc, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return MPipelineID{ _resourceMngr.CreatePipeline( INOUT desc, dbgName )};
	}
	
	RTPipelineID  VFrameGraph::CreatePipeline (INOUT RayTracingPipelineDesc &desc)
	{
		CHECK_ERR( _IsInitialized() );
		return RTPipelineID{ _resourceMngr.CreatePipeline( INOUT desc )};
	}
	
	GPipelineID  VFrameGraph::CreatePipeline (INOUT GraphicsPipelineDesc &desc, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return GPipelineID{ _resourceMngr.CreatePipeline( INOUT desc, dbgName )};
	}
	
	CPipelineID  VFrameGraph::CreatePipeline (INOUT ComputePipelineDesc &desc, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return CPipelineID{ _resourceMngr.CreatePipeline( INOUT desc, dbgName )};
	}
	
/*
=================================================
	CreateImage
=================================================
*/
	ImageID  VFrameGraph::CreateImage (const ImageDesc &desc, const MemoryDesc &mem, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );

		RawImageID	result = _resourceMngr.CreateImage( desc, mem, _GetQueuesMask( desc.queues ), VK_IMAGE_LAYOUT_MAX_ENUM, dbgName );
		
		// add first image layout transition
		if ( result )
		{
			_TransitImageLayoutToDefault( result, VK_IMAGE_LAYOUT_UNDEFINED, VK_QUEUE_FAMILY_IGNORED );
		}
		return ImageID{ result };
	}
	
/*
=================================================
	CreateBuffer
=================================================
*/
	BufferID  VFrameGraph::CreateBuffer (const BufferDesc &desc, const MemoryDesc &mem, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return BufferID{ _resourceMngr.CreateBuffer( desc, mem, _GetQueuesMask( desc.queues ), dbgName )};
	}

/*
=================================================
	CreateImage
=================================================
*/
	ImageID  VFrameGraph::CreateImage (const ExternalImageDesc_t &desc, OnExternalImageReleased_t &&onRelease, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );

		auto*	img_desc = UnionGetIf<VulkanImageDesc>( &desc );
		CHECK_ERR( img_desc );

		RawImageID	result = _resourceMngr.CreateImage( *img_desc, std::move(onRelease), dbgName );
		
		// add first image layout transition
		if ( result )
		{
			VkImageLayout	initial_layout	= BitCast<VkImageLayout>( img_desc->layout );
			
			_TransitImageLayoutToDefault( result, initial_layout, img_desc->queueFamily );
		}
		return ImageID{ result };
	}
	
/*
=================================================
	CreateBuffer
=================================================
*/
	BufferID  VFrameGraph::CreateBuffer (const ExternalBufferDesc_t &desc, OnExternalBufferReleased_t &&onRelease, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		
		auto*	buf_desc = UnionGetIf<VulkanBufferDesc>( &desc );
		CHECK_ERR( buf_desc );

		return BufferID{ _resourceMngr.CreateBuffer( *buf_desc, std::move(onRelease), dbgName )};
	}

/*
=================================================
	CreateSampler
=================================================
*/
	SamplerID  VFrameGraph::CreateSampler (const SamplerDesc &desc, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return SamplerID{ _resourceMngr.CreateSampler( desc, dbgName )};
	}
	
/*
=================================================
	CreateRayTracingShaderTable
=================================================
*/
	RTShaderTableID  VFrameGraph::CreateRayTracingShaderTable (StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return RTShaderTableID{ _resourceMngr.CreateRayTracingShaderTable( dbgName )};
	}

/*
=================================================
	_InitPipelineResources
=================================================
*/
	template <typename PplnID>
	bool  VFrameGraph::_InitPipelineResources (const PplnID &pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const
	{
		CHECK_ERR( _IsInitialized() );
		
		auto const *	ppln = _resourceMngr.GetResource( pplnId );
		CHECK_ERR( ppln );

		auto const *	ppln_layout = _resourceMngr.GetResource( ppln->GetLayoutID() );
		CHECK_ERR( ppln_layout );

		RawDescriptorSetLayoutID	layout_id;
		uint						binding;

		if ( not ppln_layout->GetDescriptorSetLayout( id, OUT layout_id, OUT binding ) )
			return false;

		VDescriptorSetLayout const*	ds_layout = _resourceMngr.GetResource( layout_id );
		CHECK_ERR( ds_layout );

		CHECK_ERR( PipelineResourcesHelper::Initialize( OUT resources, layout_id, ds_layout->GetResources() ));
		return true;
	}
	
/*
=================================================
	InitPipelineResources
=================================================
*/
	bool  VFrameGraph::InitPipelineResources (RawGPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const
	{
		return _InitPipelineResources( pplnId, id, OUT resources );
	}

	bool  VFrameGraph::InitPipelineResources (RawCPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const
	{
		return _InitPipelineResources( pplnId, id, OUT resources );
	}

	bool  VFrameGraph::InitPipelineResources (RawMPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const
	{
		return _InitPipelineResources( pplnId, id, OUT resources );
	}

	bool  VFrameGraph::InitPipelineResources (RawRTPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const
	{
		return _InitPipelineResources( pplnId, id, OUT resources );
	}
	
/*
=================================================
	CachePipelineResources
=================================================
*/
	bool  VFrameGraph::CachePipelineResources (INOUT PipelineResources &resources)
	{
		CHECK_ERR( _IsInitialized() );
		return _resourceMngr.CacheDescriptorSet( INOUT resources );
	}
	
/*
=================================================
	CreateSwapchain
=================================================
*/
	SwapchainID  VFrameGraph::CreateSwapchain (const SwapchainCreateInfo_t &desc, RawSwapchainID oldSwapchain, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );

		return SwapchainID{ Visit( desc,
						[&] (const VulkanSwapchainCreateInfo &info)	{ return _resourceMngr.CreateSwapchain( info, oldSwapchain, *this, dbgName ); },
						[] (const auto &)							{ ASSERT( !"not supported" ); return RawSwapchainID{}; }
					)};
	}

/*
=================================================
	CreateRayTracingGeometry
=================================================
*/
	RTGeometryID  VFrameGraph::CreateRayTracingGeometry (const RayTracingGeometryDesc &desc, const MemoryDesc &mem, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return RTGeometryID{ _resourceMngr.CreateRayTracingGeometry( desc, mem, dbgName )};
	}
	
/*
=================================================
	CreateRayTracingScene
=================================================
*/
	RTSceneID  VFrameGraph::CreateRayTracingScene (const RayTracingSceneDesc &desc, const MemoryDesc &mem, StringView dbgName)
	{
		CHECK_ERR( _IsInitialized() );
		return RTSceneID{ _resourceMngr.CreateRayTracingScene( desc, mem, dbgName )};
	}

/*
=================================================
	_ReleaseResource
=================================================
*/
	template <typename ID>
	inline void VFrameGraph::_ReleaseResource (INOUT ID &id)
	{
		if ( not id )
			return;

		CHECK_ERR( _IsInitialized(), void());
		return _resourceMngr.ReleaseResource( id.Release() );
	}
	
/*
=================================================
	ReleaseResource
=================================================
*/
	void VFrameGraph::ReleaseResource (INOUT GPipelineID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT CPipelineID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT MPipelineID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT RTPipelineID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT ImageID &id)			{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT BufferID &id)			{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT SamplerID &id)			{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT SwapchainID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT RTGeometryID &id)		{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT RTSceneID &id)			{ _ReleaseResource( INOUT id ); }
	void VFrameGraph::ReleaseResource (INOUT RTShaderTableID &id)	{ _ReleaseResource( INOUT id ); }
	
/*
=================================================
	ReleaseResource
=================================================
*/
	void VFrameGraph::ReleaseResource (INOUT PipelineResources &resources)
	{
		CHECK_ERR( _IsInitialized(), void());
		return _resourceMngr.ReleaseResource( INOUT resources );
	}

/*
=================================================
	_GetDescription
=================================================
*/
	template <typename Desc, typename ID>
	inline Desc const&  VFrameGraph::_GetDescription (const ID &id) const
	{
		ASSERT( _IsInitialized() );

		// read access available without synchronizations
		return _resourceMngr.GetDescription( id );
	}
	
/*
=================================================
	GetDescription
=================================================
*/
	BufferDesc const&  VFrameGraph::GetDescription (RawBufferID id) const
	{
		return _GetDescription<BufferDesc>( id );
	}

	ImageDesc const&  VFrameGraph::GetDescription (RawImageID id) const
	{
		return _GetDescription<ImageDesc>( id );
	}
	
	/*SamplerDesc const&  VFrameGraph::GetDescription (RawSamplerID id) const
	{
		return _GetDescription<SamplerDesc>( id );
	}*/

/*
=================================================
	UpdateHostBuffer
=================================================
*/
	bool  VFrameGraph::UpdateHostBuffer (RawBufferID id, BytesU offset, BytesU size, const void *data)
	{
		void*	dst_ptr;
		CHECK_ERR( MapBufferRange( id, offset, INOUT size, OUT dst_ptr ));

		MemCopy( OUT dst_ptr, size, data, size );
		return true;
	}
	
/*
=================================================
	MapBufferRange
=================================================
*/
	bool  VFrameGraph::MapBufferRange (RawBufferID id, BytesU offset, INOUT BytesU &size, OUT void* &dataPtr)
	{
		VBuffer const*		buffer = _resourceMngr.GetResource( id );
		CHECK_ERR( buffer );

		VMemoryObj const*	memory = _resourceMngr.GetResource( buffer->GetMemoryID() );
		CHECK_ERR( memory );

		VMemoryObj::MemoryInfo	mem_info;
		CHECK_ERR( memory->GetInfo( _resourceMngr.GetMemoryManager(), OUT mem_info ));

		CHECK_ERR( mem_info.mappedPtr );
		CHECK_ERR( offset < buffer->Size() );

		size	= Min( size, mem_info.size - offset );
		dataPtr = mem_info.mappedPtr + offset;

		//if ( _debugger )
		//	_debugger->AddHostWriteAccess( buffer->ToGlobal(), offset, size );
		
		return true;
	}
	
/*
=================================================
	Begin
=================================================
*/
	CommandBuffer  VFrameGraph::Begin (const CommandBufferDesc &desc, ArrayView<CommandBuffer> dependsOn)
	{
		EXLOCK( _cmdBuffersGuard );
		CHECK_ERR( uint(desc.queueType) < _queueMap.size() );
		
		VCommandBuffer*	cmd		= null;
		VCmdBatch*		batch	= null;
		auto&			queue	= _queueMap[ uint(desc.queueType) ];

		// try to free already submitted commands
		if ( queue.submitted.size() )
		{
			auto*	submitted	= queue.submitted.front();
			auto	fence		= submitted->GetFence();

			if ( not fence or _device.vkGetFenceStatus( _device.GetVkDevice(), fence ) == VK_SUCCESS )
			{
				VK_CALL( _device.vkResetFences( _device.GetVkDevice(), 1, &fence ));

				submitted->_Release( GetDevice(), _debugger, _shaderDebugCallback, OUT _semaphoreCache, OUT _fenceCache );

				queue.submitted.erase( queue.submitted.begin() );
				_submittedPool.Unassign( submitted->GetIndexInPool() );
			}
		}

		// acquire command buffer
		for (;;)
		{
			uint	cmd_index;
			if ( _cmdBufferPool.Assign( OUT cmd_index,
									    [this](VCommandBuffer *ptr, uint idx){ PlacementNew<VCommandBuffer>( ptr, *this, idx ); }) )
			{
				cmd = &_cmdBufferPool[cmd_index];
				break;
			}
			
			ASSERT( !"overflow" );
			std::this_thread::yield();
		}

		// acquire command batch
		for (;;)
		{
			uint	batch_index;
			if ( _cmdBatchPool.Assign( OUT batch_index,
									   [this](VCmdBatch *ptr, uint idx){ PlacementNew<VCmdBatch>( ptr, *this, idx ); }) )
			{
				batch = &_cmdBatchPool[batch_index];
				batch->Initialize( queue.ptr, desc.queueType, dependsOn );
				break;
			}
			
			ASSERT( !"overflow" );
			std::this_thread::yield();
		}

		CHECK_ERR( cmd->Begin( desc, batch ));

		return CommandBuffer{ cmd, batch };
	}
	
/*
=================================================
	Execute
=================================================
*/
	bool  VFrameGraph::Execute (INOUT CommandBuffer &cmdBufPtr)
	{
		CHECK_ERR( cmdBufPtr.GetCommandBuffer() and cmdBufPtr.GetBatch() );
		EXLOCK( _cmdBuffersGuard );

		VCommandBuffer*	cmd		= Cast<VCommandBuffer>(cmdBufPtr.GetCommandBuffer());
		VCmdBatchPtr	batch	= cmd->GetBatchPtr();
		CHECK_ERR( batch.get() == cmdBufPtr.GetBatch() );

		CHECK_ERR( cmd->Execute() );
		_cmdBufferPool.Unassign( cmd->GetIndexInPool() );

		cmdBufPtr = CommandBuffer{ (ICommandBuffer*)(null), cmdBufPtr.GetBatch() };

		// add batch to the submission queue
		uint	q_idx = uint(batch->GetQueueType());
		CHECK_ERR( q_idx < _queueMap.size() );

		_queueMap[q_idx].pending.push_back( batch );

		//_FlushQueue( batch->GetQueueUsage(), 3u );
		return true;
	}
	
/*
=================================================
	_CreateFence
=================================================
*/
	VkFence  VFrameGraph::_CreateFence ()
	{
		if ( _fenceCache.size() )
		{
			auto	result = _fenceCache.back();
			_fenceCache.pop_back();
			return result;
		}

		VkFenceCreateInfo	info	= {};
		VkFence				result	= VK_NULL_HANDLE;

		info.sType	= VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		info.flags	= 0;	// VK_FENCE_CREATE_SIGNALED_BIT

		VK_CHECK( _device.vkCreateFence( _device.GetVkDevice(), &info, null, OUT &result ));
		_device.SetObjectName( uint64_t(result), "BatchFence", VK_OBJECT_TYPE_FENCE );

		return result;
	}
	
/*
=================================================
	_CreateSemaphore
=================================================
*/
	VkSemaphore  VFrameGraph::_CreateSemaphore ()
	{
		if ( _semaphoreCache.size() )
		{
			auto	result = _semaphoreCache.back();
			_semaphoreCache.pop_back();
			return result;
		}

		VkSemaphoreCreateInfo	info	= {};
		VkSemaphore				result	= VK_NULL_HANDLE;

		info.sType	= VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		info.flags	= 0;

		VK_CHECK( _device.vkCreateSemaphore( _device.GetVkDevice(), &info, null, OUT &result ));
		_device.SetObjectName( uint64_t(result), "BatchSemaphore", VK_OBJECT_TYPE_SEMAPHORE );

		return result;
	}

/*
=================================================
	Flush
=================================================
*/
	bool  VFrameGraph::Flush ()
	{
		EXLOCK( _cmdBuffersGuard );

		return _FlushAll( 10u );
	}
	
/*
=================================================
	_FlushAll
=================================================
*/
	bool  VFrameGraph::_FlushAll (uint maxIter)
	{
		for (size_t a = 0, a_max = Min( maxIter, _queueMap.size()), changed = 1;
			 changed and (a < a_max);
			 ++a)
		{
			changed = 0;

			// for each queue type
			for (size_t qi = 0; qi < _queueMap.size(); ++qi)
			{
				if ( _queueMap[qi].ptr )
					changed |= size_t(_FlushQueue( EQueueType(qi), 10u ));
			}
		}

		return true;
	}
	
/*
=================================================
	_FlushQueue
=================================================
*/
	bool  VFrameGraph::_FlushQueue (EQueueType queueIndex, uint maxIter)
	{
		uint				qi		= uint(queueIndex);
		auto&				q		= _queueMap[qi];
		EQueueUsage			q_mask	= Default;

		CmdBatches_t		pending;
		SubmitInfos_t		submit_infos;
		TempSemaphores_t	temp_semaphores;
		PendingSwapchains_t	swapchains;

		for (size_t b = 0, b_max = Min( maxIter, q.pending.size()), changed = 1;
			 changed and (b < b_max);
			 ++b)
		{
			changed = 0;

			for (auto iter = q.pending.begin(); iter != q.pending.end();)
			{
				auto&		batch	 = *iter;
				auto		state	 = batch->GetState();
				bool		is_ready = true;
				EQueueUsage	q_mask2	 = Default;

				ASSERT( state == EBatchState::Backed );
				
				for (auto& dep : batch->GetDependencies())
				{
					q_mask2		|= dep->GetQueueType();
					is_ready	&= (dep->GetState() >= EBatchState::Ready);
				}

				if ( is_ready )
				{
					batch->OnReadyToSubmit();
					pending.push_back( std::move(batch) );
							
					changed	= 1;
					iter	= q.pending.erase( iter );
					q_mask	|= q_mask2;
				}
				else
					++iter;
			}
		}
		
		if ( pending.empty() )
			return false;

		// add semaphores
		for (size_t qj = 0; qj < _queueMap.size(); ++qj)
		{
			auto&	q2 = _queueMap[qj];

			if ( not q2.ptr or qi == qj )
				continue;
						
			// input
			if ( EnumEq( q_mask, 1u<<qj ) and q2.semaphores[qi] )
			{
				pending.front()->WaitSemaphore( q2.semaphores[qi], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
				temp_semaphores.push_back( q2.semaphores[qi] );
				q2.semaphores[qi] = VK_NULL_HANDLE;
			}
						
			// output
			{
				if ( q.semaphores[qj] )
					temp_semaphores.push_back( q.semaphores[qj] );

				VkSemaphore	sem = _CreateSemaphore();

				pending.back()->SignalSemaphore( sem );
				q.semaphores[qj] = sem;
			}
		}
					
		q.lastSubmitted = ExeOrderIndex(uint(q.lastSubmitted) + 1);

		VSubmitted*	submit = null;
		for (;;)
		{
			uint	index;
			if ( _submittedPool.Assign( OUT index, [](VSubmitted* ptr, uint idx) { PlacementNew<VSubmitted>( ptr, idx ); }) )
			{
				submit = &_submittedPool[index];
				submit->Initialize( EQueueType(qi), pending, temp_semaphores, _CreateFence(), q.lastSubmitted );
				q.submitted.push_back( submit );
				break;
			}
			
			ASSERT( !"overflow" );
			std::this_thread::yield();
		}

		// add image layout transitions
		if ( q.imageBarriers.size() )
		{
			VkCommandBuffer  cmdbuf = q.cmdPool.AllocPrimary( _device );
				
			VkCommandBufferBeginInfo	begin = {};
			begin.sType		= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags		= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VK_CHECK( _device.vkBeginCommandBuffer( cmdbuf, &begin ));

			_device.vkCmdPipelineBarrier( cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
										  0, null, 0, null, uint(q.imageBarriers.size()), q.imageBarriers.data() );

			VK_CHECK( _device.vkEndCommandBuffer( cmdbuf ));
			q.imageBarriers.clear();

			pending.front()->PushFrontCommandBuffer( cmdbuf, &q.cmdPool );
		}

		// init submit info
		for (uint i = 0; i < pending.size(); ++i)
		{
			auto&	batch = *pending[i].get();

			batch.OnSubmit( OUT submit_infos[i], OUT swapchains, submit );
			ASSERT( q.ptr == batch.GetQueue() );
		}

		// submit & present
		{
			EXLOCK( q.ptr->guard );
			VK_CALL( _device.vkQueueSubmit( q.ptr->handle, uint(pending.size()), submit_infos.data(), OUT submit->GetFence() ));
			
			for (auto* sw : swapchains)
			{
				ASSERT( q.ptr == sw->GetPresentQueue() );
				CHECK( sw->Present( _device ));
			}

			_resourceMngr.OnSubmit();
		}
		return true;
	}

/*
=================================================
	Wait
=================================================
*/
	bool  VFrameGraph::Wait (ArrayView<CommandBuffer> commands, Nanoseconds timeout)
	{
		EXLOCK( _cmdBuffersGuard );

		TempFences_t	fences;

		for (auto& cmd : commands)
		{
			auto*	batch = Cast<VCmdBatch>(cmd.GetBatch());
			CHECK_ERR( batch );

			auto	state		= batch->GetState();
			auto*	submitted	= batch->GetSubmitted();

			if ( state == EBatchState::Complete )
			{}
			else
			if ( state == EBatchState::Submitted )
			{
				auto	fence = submitted->GetFence();
				bool	found = false;

				ASSERT( fence );

				for (auto& f : fences) {
					found |= (f == fence);
				}

				if ( not found )
					fences.push_back( fence );
			}
		}

		bool	result = true;

		if ( fences.size() )
		{
			auto  res = _device.vkWaitForFences( _device.GetVkDevice(), uint(fences.size()), fences.data(), VK_TRUE, timeout.count() );

			if ( res == VK_SUCCESS )
			{
				VK_CALL( _device.vkResetFences( _device.GetVkDevice(), uint(fences.size()), fences.data() ));

				// release resources
				for (auto& cmd : commands)
				{
					if ( auto*  submitted = Cast<VCmdBatch>(cmd.GetBatch())->GetSubmitted() )
						CHECK( submitted->_Release( GetDevice(), _debugger, _shaderDebugCallback, OUT _semaphoreCache, OUT _fenceCache ));
				}
			}
			else
			{
				result = false;
				CHECK( res == VK_TIMEOUT );
			}
		}

		return result;
	}

/*
=================================================
	WaitIdle
=================================================
*/
	bool  VFrameGraph::WaitIdle ()
	{
		EXLOCK( _cmdBuffersGuard );

		TempFences_t	fences;

		CHECK_ERR( _FlushAll( 10u ));
		
		for (size_t i = 0; i < _queueMap.size(); ++i)
		{
			auto&	q = _queueMap[i];

			CHECK( q.pending.empty() );

			for (auto& s : q.submitted)
			{
				if ( auto fence = s->GetFence() )
					fences.push_back( fence );
			}
		}
		
		if ( fences.size() )
		{
			VK_CALL( _device.vkWaitForFences( _device.GetVkDevice(), uint(fences.size()), fences.data(), VK_TRUE, UMax ));
			VK_CALL( _device.vkResetFences( _device.GetVkDevice(), uint(fences.size()), fences.data() ));
		}
		
		for (auto& q : _queueMap)
		{
			for (auto* s : q.submitted) {
				CHECK( s->_Release( GetDevice(), _debugger, _shaderDebugCallback, OUT _semaphoreCache, OUT _fenceCache ));
				_submittedPool.Unassign( s->GetIndexInPool() );
			}
			q.submitted.clear();
		}

		return true;
	}
	
/*
=================================================
	_WaitQueue
=================================================
*/
	bool  VFrameGraph::_WaitQueue (EQueueType queue, Nanoseconds timeout)
	{
		TempFences_t	fences;

		auto&	q = _queueMap[ uint(queue) ];

		for (auto* s : q.submitted)
		{
			ASSERT( s->GetFence() );
			fences.push_back( s->GetFence() );
		}
		
		if ( fences.size() )
		{
			VK_CALL( _device.vkWaitForFences( _device.GetVkDevice(), uint(fences.size()), fences.data(), VK_TRUE, UMax ));
			VK_CALL( _device.vkResetFences( _device.GetVkDevice(), uint(fences.size()), fences.data() ));
		}
		
		for (auto* s : q.submitted) {
			CHECK( s->_Release( GetDevice(), _debugger, _shaderDebugCallback, OUT _semaphoreCache, OUT _fenceCache ));
			_submittedPool.Unassign( s->GetIndexInPool() );
		}
		q.submitted.clear();

		return true;
	}

/*
=================================================
	GetStatistics
=================================================
*/
	bool  VFrameGraph::GetStatistics (OUT Statistics &result) const
	{
		return false;
	}
	
/*
=================================================
	DumpToString
=================================================
*/
	bool  VFrameGraph::DumpToString (OUT String &result) const
	{
		_debugger.GetFrameDump( OUT result );
		return true;
	}
	
/*
=================================================
	DumpToGraphViz
=================================================
*/
	bool  VFrameGraph::DumpToGraphViz (OUT String &result) const
	{
		_debugger.GetGraphDump( OUT result );
		return true;
	}
	
/*
=================================================
	_IsUnique
=================================================
*/
	bool  VFrameGraph::_IsUnique (VDeviceQueueInfoPtr ptr) const
	{
		for (auto& q : _queueMap) {
			if ( q.ptr == ptr )
				return false;
		}
		return true;
	}
	
/*
=================================================
	_CreateQueue
=================================================
*/
	bool  VFrameGraph::_CreateQueue (EQueueType queueIndex, VDeviceQueueInfoPtr queuePtr)
	{
		_queueUsage |= queueIndex;

		auto&	q = _queueMap[ uint(queueIndex) ];

		q.ptr = queuePtr;

		CHECK_ERR( q.cmdPool.Create( _device, q.ptr ));

		return true;
	}

/*
=================================================
	_AddGraphicsQueue
=================================================
*/
	bool  VFrameGraph::_AddGraphicsQueue ()
	{
		VDeviceQueueInfoPtr		best_match;
		VDeviceQueueInfoPtr		compatible;

		for (auto& queue : _device.GetVkQueues())
		{
			const bool	is_unique		= _IsUnique( &queue );
			const bool	has_graphics	= EnumEq( queue.familyFlags, VK_QUEUE_GRAPHICS_BIT );

			if ( has_graphics )
			{
				compatible = &queue;

				if ( is_unique ) {
					best_match = &queue;
					break;
				}
			}
		}
		
		if ( not best_match )
			best_match = compatible;

		if ( best_match )
			return _CreateQueue( EQueueType::Graphics, best_match );
		
		return false;
	}
	
/*
=================================================
	_AddAsyncComputeQueue
=================================================
*/
	bool  VFrameGraph::_AddAsyncComputeQueue ()
	{
		VDeviceQueueInfoPtr		unique;
		VDeviceQueueInfoPtr		best_match;
		VDeviceQueueInfoPtr		compatible;

		for (auto& queue : _device.GetVkQueues())
		{
			const bool	is_unique		= _IsUnique( &queue );
			const bool	has_compute		= EnumEq( queue.familyFlags, VK_QUEUE_COMPUTE_BIT );
			const bool	has_graphics	= EnumEq( queue.familyFlags, VK_QUEUE_GRAPHICS_BIT );

			// compute without graphics
			if ( has_compute and not has_graphics )
			{
				compatible = &queue;

				if ( is_unique ) {
					best_match = &queue;
					break;
				}
			}
			else
			
			// any unique queue that supports compute
			if ( (has_compute or has_graphics) and is_unique )
			{
				unique = &queue;
			}
		}

		// unique compute/graphics queue is better than non-unique compute queue
		if ( not best_match )
			best_match = unique;
		
		if ( not best_match )
			best_match = compatible;
		
		if ( best_match )
			return _CreateQueue( EQueueType::AsyncCompute, best_match );
		
		return false;
	}
	
/*
=================================================
	_AddAsyncTransferQueue
=================================================
*/
	bool  VFrameGraph::_AddAsyncTransferQueue ()
	{
		VDeviceQueueInfoPtr		unique;
		VDeviceQueueInfoPtr		best_match;
		VDeviceQueueInfoPtr		compatible;

		for (auto& queue : _device.GetVkQueues())
		{
			const bool	is_unique			= _IsUnique( &queue );
			const bool	has_transfer		= EnumEq( queue.familyFlags, VK_QUEUE_TRANSFER_BIT );
			const bool	supports_transfer	= EnumAny( queue.familyFlags, VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT );

			// transfer without graphics or compute
			if ( has_transfer and not supports_transfer )
			{
				compatible = &queue;

				if ( is_unique ) {
					best_match = &queue;
					break;
				}
			}
			else
			
			// any unique queue that supports transfer
			if ( (has_transfer or supports_transfer) and is_unique )
			{
				unique = &queue;
			}
		}
		
		// unique compute/graphics queue is better than non-unique transfer queue
		if ( not best_match )
			best_match = unique;
		
		if ( not best_match )
			best_match = compatible;
		
		if ( best_match )
			return _CreateQueue( EQueueType::AsyncTransfer, best_match );
		
		return false;
	}

/*
=================================================
	FindQueue
=================================================
*/
	VDeviceQueueInfoPtr  VFrameGraph::FindQueue (EQueueType type) const
	{
		return uint(type) < _queueMap.size() ? _queueMap[ uint(type) ].ptr : null;
	}

/*
=================================================
	_GetQueuesMask
=================================================
*/
	EQueueFamilyMask  VFrameGraph::_GetQueuesMask (EQueueUsage types) const
	{
		EQueueFamilyMask	mask = Default;

		for (uint i = 0; (1u<<i) <= uint(types); ++i)
		{
			if ( not EnumEq( types, 1u<<i ) )
				continue;

			mask |= _queueMap[i].ptr->familyIndex;
		}
		return mask;
	}
	
/*
=================================================
	_IsInitialized / _GetState / _SetState
=================================================
*/
	inline bool  VFrameGraph::_IsInitialized () const
	{
		return _state.load( memory_order_relaxed ) == EState::Idle;
	}

	inline VFrameGraph::EState  VFrameGraph::_GetState () const
	{
		return _state.load( memory_order_acquire );
	}

	inline bool  VFrameGraph::_SetState (EState expected, EState newState)
	{
		return _state.compare_exchange_strong( INOUT expected, newState, memory_order_release, memory_order_relaxed );
	}
	
/*
=================================================
	RecycleBatch
=================================================
*/
	void  VFrameGraph::RecycleBatch (const VCmdBatch *batch)
	{
		_cmdBatchPool.Unassign( batch->GetIndexInPool() );
	}


}	// FG
