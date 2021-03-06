// Copyright (c) 2018-2020,  Zhirnov Andrey. For more information see 'LICENSE'

#include "../FGApp.h"

namespace FG
{

	bool FGApp::Test_ExternalCmdBuf1 ()
	{
		const BytesU	src_buffer_size = 256_b;
		const BytesU	dst_buffer_size = 512_b;

		// create vulkan command pool & buffers
		VkCommandPool		vk_cmdpool	= VK_NULL_HANDLE;
		VkCommandBuffer		vk_cmdbuf	= VK_NULL_HANDLE;
		{
			VkCommandPoolCreateInfo		info = {};
			info.sType				= VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			info.queueFamilyIndex	= _vulkan.GetVkQueues()[0].familyIndex;
			info.flags				= 0;

			VK_CHECK( _vulkan.vkCreateCommandPool( _vulkan.GetVkDevice(), &info, null, OUT &vk_cmdpool ));

			VkCommandBufferAllocateInfo		alloc = {};
			alloc.sType					= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			alloc.level					= VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			alloc.commandPool			= vk_cmdpool;
			alloc.commandBufferCount	= 1;

			VK_CHECK( _vulkan.vkAllocateCommandBuffers( _vulkan.GetVkDevice(), &alloc, OUT &vk_cmdbuf ));
		}

		// create vulkan buffer & memory
		VkBuffer			vk_buffer	= VK_NULL_HANDLE;
		VkDeviceMemory		vk_memory	= VK_NULL_HANDLE;
		VulkanBufferDesc	vk_buf_desc	= {};
		{
			VkBufferCreateInfo	info = {};
			info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			info.size			= VkDeviceSize(dst_buffer_size);
			info.usage			= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

			VK_CHECK( _vulkan.vkCreateBuffer( _vulkan.GetVkDevice(), &info, null, OUT &vk_buffer ));

			VkMemoryRequirements	mem_req = {};
			_vulkan.vkGetBufferMemoryRequirements( _vulkan.GetVkDevice(), vk_buffer, OUT &mem_req );

			VkMemoryAllocateInfo	alloc = {};
			alloc.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc.allocationSize	= mem_req.size;

			CHECK_ERR( _vulkan.GetMemoryTypeIndex( mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, OUT alloc.memoryTypeIndex ));
			VK_CHECK( _vulkan.vkAllocateMemory( _vulkan.GetVkDevice(), &alloc, null, OUT &vk_memory ));

			VK_CHECK( _vulkan.vkBindBufferMemory( _vulkan.GetVkDevice(), vk_buffer, vk_memory, 0 ));

			vk_buf_desc.buffer	= BitCast<BufferVk_t>( vk_buffer );
			vk_buf_desc.usage	= BitCast<BufferUsageFlagsVk_t>( info.usage );
			vk_buf_desc.size	= BytesU{ info.size };
		}
		
		BufferID		src_buffer	= _frameGraph->CreateBuffer( BufferDesc{ src_buffer_size, EBufferUsage::Transfer }, Default, "SrcBuffer" );
		BufferID		dst_buffer	= _frameGraph->CreateBuffer( BufferDesc{ dst_buffer_size, EBufferUsage::Transfer }, Default, "DstBuffer" );
		BufferID		ext_buffer	= _frameGraph->CreateBuffer( vk_buf_desc, Default, "ExternalBuffer" );

		Array<uint8_t>	src_data;	src_data.resize( size_t(src_buffer_size) );

		for (size_t i = 0; i < src_data.size(); ++i) {
			src_data[i] = uint8_t(i);
		}

		bool	cb_was_called	= false;
		bool	data_is_correct	= false;

		const auto	OnLoaded = [&src_data, dst_buffer_size, OUT &cb_was_called, OUT &data_is_correct] (BufferView data)
		{
			cb_was_called	= true;
			data_is_correct	= (data.size() == size_t(dst_buffer_size));

			for (size_t i = 0; i < src_data.size(); ++i)
			{
				bool	is_equal = (src_data[i] == data[i+64]);
				ASSERT( is_equal );

				data_is_correct &= is_equal;
			}
		};
		

		CommandBuffer	cmd1 = _frameGraph->Begin( CommandBufferDesc{}.SetDebugFlags( EDebugFlags::Default ) );
		CommandBuffer	cmd2 = _frameGraph->Begin( CommandBufferDesc{}.SetDebugFlags( EDebugFlags::Default ), {cmd1} );
		CommandBuffer	cmd3 = _frameGraph->Begin( CommandBufferDesc{}.SetDebugFlags( EDebugFlags::Default ), {cmd2} );
		CHECK_ERR( cmd1 and cmd2 and cmd3 );

		// thread 1
		{
			Task	t_update	= cmd1->AddTask( UpdateBuffer().SetBuffer( src_buffer ).AddData( ArrayView<uint8_t>{src_data}.section( 0, 128 ) ));
			Task	t_copy		= cmd1->AddTask( CopyBuffer().From( src_buffer ).To( dst_buffer ).AddRegion( 0_b, 64_b, 128_b ).DependsOn( t_update ));
			FG_UNUSED( t_copy );

			CHECK_ERR( _frameGraph->Execute( cmd1 ));
		}

		// thread 2
		{
			VkCommandBufferBeginInfo	begin = {};
			begin.sType		= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags		= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VK_CALL( _vulkan.vkBeginCommandBuffer( vk_cmdbuf, &begin ));

			_vulkan.vkCmdUpdateBuffer( vk_cmdbuf, vk_buffer, 128, 128, src_data.data() + 128_b );

			VkBufferMemoryBarrier	barrier = {};
			barrier.sType				= VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.srcAccessMask		= VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask		= 0;
			barrier.srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer				= vk_buffer;
			barrier.offset				= 128;
			barrier.size				= 256;

			_vulkan.vkCmdPipelineBarrier( vk_cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
										  0, null, 1, &barrier, 0, null );

			VK_CALL( _vulkan.vkEndCommandBuffer( vk_cmdbuf ));

			VulkanCommandBatch	vk_cmdbatch = {};
			vk_cmdbatch.commands			= { Cast<CommandBufferVk_t>( &vk_cmdbuf ), 1 };
			vk_cmdbatch.queueFamilyIndex	= _vulkan.GetVkQueues()[0].familyIndex;

			CHECK_ERR( cmd2->AddExternalCommands( vk_cmdbatch ));
			CHECK_ERR( _frameGraph->Execute( cmd2 ));
		}

		// thread 3
		{
			Task	t_copy	= cmd3->AddTask( CopyBuffer().From( ext_buffer ).To( dst_buffer ).AddRegion( 128_b, 192_b, 128_b ));
			Task	t_read	= cmd3->AddTask( ReadBuffer().SetBuffer( dst_buffer, 0_b, dst_buffer_size ).SetCallback( OnLoaded ).DependsOn( t_copy ));
			FG_UNUSED( t_read );
			
			CHECK_ERR( _frameGraph->Execute( cmd3 ));
		}
		
		CHECK_ERR( not cb_was_called );
		CHECK_ERR( _frameGraph->WaitIdle() );

		CHECK_ERR( CompareDumps( TEST_NAME ));
		CHECK_ERR( Visualize( TEST_NAME ));
		
		CHECK_ERR( cb_was_called );
		CHECK_ERR( data_is_correct );

		// destroy vulkan objects
		{
			_vulkan.vkDestroyBuffer( _vulkan.GetVkDevice(), vk_buffer, null );
			_vulkan.vkFreeMemory( _vulkan.GetVkDevice(), vk_memory, null );
			_vulkan.vkDestroyCommandPool( _vulkan.GetVkDevice(), vk_cmdpool, null );
		}

		DeleteResources( src_buffer, dst_buffer, ext_buffer );

		FG_LOGI( TEST_NAME << " - passed" );
		return true;
	}

}	// FG
