#include "pch.h"
#pragma optimize("",off)
#include "uc_uwp_renderer_impl.h"

#include <array>

#include "uc_uwp_renderer_impl_window.h"

#include <ppl.h>
#include "uc_uwp_ui_helper.h"

#define UWP
#include <uc/gx/pinhole_camera.h>
#include <uc/gx/lip/lip.h>
#include <uc/gx/lip_utils.h>
#include "uwp/file.h"





using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::Graphics::Display;

namespace
{
	size_t align(size_t v, size_t a)
	{
		return (v + a - 1) & (~(a - 1));
	}
}

namespace uc
{
    namespace uwp
    {
        renderer_impl::renderer_impl( bool* window_close, const winrt::Windows::UI::Core::CoreWindow& window, const winrt::Windows::Graphics::Display::DisplayInformation& display_information) : m_main_window(window_close)
        {
            set_window(window);
            set_display_info(display_information);
            
            m_resources.add_swapchain(swap_chain::swap_chain_type::background_core_window);
            m_resources.add_swapchain(swap_chain::swap_chain_type::foreground_core_window);
        }

        void renderer_impl::initialize_resources()
        {
            using namespace gx::dx12;

			auto mesh		= lip::create_from_compressed_lip_file<lip::derivatives_skinned_model>(L"Assets\\models\\military_mechanic.derivatives_skinned_model.model");
			auto pos		= static_cast<uint32_t>(align(size(mesh->m_positions), 256U));
			auto uv			= static_cast<uint32_t>(align(size(mesh->m_uv), 256U));
			auto normals	= static_cast<uint32_t>(align(size(mesh->m_normals), 256UL));
			auto tangents	= static_cast<uint32_t>(align(size(mesh->m_tangents), 256UL));
			auto indices	= static_cast<uint32_t>(align(size(mesh->m_indices), 256UL));

			gpu_resource_create_context* rc = m_resources.resource_create_context();

			gx::dx12::managed_graphics_command_context ctx = create_graphics_command_context(m_resources.direct_command_context_allocator(device_resources::swap_chains::background));

			m_mesh_opaque.m_opaque_textures.resize(mesh->m_textures.size());
			m_mesh_opaque.m_opaque_ranges.resize(mesh->m_primitive_ranges.size());

			for (auto i = 0U; i < mesh->m_textures.size(); ++i)
			{
				const auto& texture = mesh->m_textures[i];

				auto w = texture.m_levels[0].m_width;
				auto h = texture.m_levels[0].m_height;

				m_mesh_opaque.m_opaque_textures[i]	 = gx::dx12::create_texture_2d(rc, w, h, static_cast<DXGI_FORMAT>(texture.m_levels[0].view()), D3D12_RESOURCE_STATE_COPY_DEST);
				D3D12_SUBRESOURCE_DATA s[1];
				s[0] = gx::sub_resource_data(&texture.m_levels[0]);
				ctx->upload_resource(m_mesh_opaque.m_opaque_textures[i].get(), 0, 1, &s[0]);
			}

			for (auto i = 0U; i < mesh->m_primitive_ranges.size(); ++i)
			{
				const auto& r = mesh->m_primitive_ranges[i];
				m_mesh_opaque.m_opaque_ranges[i].m_begin = r.m_begin;
				m_mesh_opaque.m_opaque_ranges[i].m_end	 = r.m_end;
			}

			auto s					= static_cast<uint32_t > (pos + uv + normals + tangents + indices);
			m_geometry				= gx::dx12::create_byteaddress_buffer(rc, s, D3D12_RESOURCE_STATE_COPY_DEST);

			//allocation
			m_mesh.m_pos			= 0;
			m_mesh.m_uv				= pos;
			m_mesh.m_normals		= pos + uv;
			m_mesh.m_tangents		= pos + uv + normals;
			m_mesh.m_indices		= pos + uv + normals + tangents;
			m_mesh.m_indices_size	= static_cast<uint32_t>(size(mesh->m_indices));
			m_mesh.m_vertex_count	= static_cast<uint32_t>(mesh->m_positions.size());

			ctx->upload_buffer(m_geometry.get(), m_mesh.m_pos,		mesh->m_positions.data(),	size(mesh->m_positions));
			ctx->upload_buffer(m_geometry.get(), m_mesh.m_uv,		mesh->m_uv.data(),			size(mesh->m_uv));
			ctx->upload_buffer(m_geometry.get(), m_mesh.m_normals,	mesh->m_normals.data(),		size(mesh->m_normals));
			ctx->upload_buffer(m_geometry.get(), m_mesh.m_tangents, mesh->m_tangents.data(),	size(mesh->m_tangents));
			ctx->upload_buffer(m_geometry.get(), m_mesh.m_indices,	mesh->m_indices.data(),		size(mesh->m_indices));

			ctx->transition_resource(m_geometry.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			for (auto&& t : m_mesh_opaque.m_opaque_textures)
			{
				ctx->transition_resource(t.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}

			ctx->submit();

            //flush all uploaded resources previous frame
            m_resources.direct_queue(device_resources::swap_chains::background )->insert_wait_on(m_resources.upload_queue()->flush());
            m_resources.direct_queue(device_resources::swap_chains::background )->insert_wait_on(m_resources.compute_queue()->signal_fence());

			m_resources.wait_for_gpu();

			m_solid_graphics		= gx::dx12::solid_graphics::create_pso(m_resources.device_d2d12(), rc->null_cbv(), rc->null_srv(), rc->null_uav(), rc->null_sampler() );
			m_solid_graphics_depth	= gx::dx12::solid_graphics_depth::create_pso(m_resources.device_d2d12(), rc->null_cbv(), rc->null_srv(), rc->null_uav(), rc->null_sampler());
        }

        static inline uint64_t next_frame(uint64_t frame)
        {
            uint64_t r = frame + 1;
            return r % 3;
        }

        void renderer_impl::set_display_info(const winrt::Windows::Graphics::Display::DisplayInformation& display_information)
        {
            m_display_information = display_information;
        }

        void renderer_impl::set_window( const winrt::Windows::UI::Core::CoreWindow& window)
        {
            m_window = window;
        }

        void renderer_impl::set_swapchainpanel(const winrt::Windows::UI::Xaml::Controls::SwapChainPanel&)
        {

        }

        void renderer_impl::refresh_display_layout()
        {
            resize_window_command c;
            c.m_window_environment = build_environment(m_window, m_display_information);
            m_prerender_queue.push(std::move(c));
            //ui stuff
            m_mouse.set_window(m_window, c.m_window_environment.m_effective_dpi);
            m_keyboard.set_window(m_window);
        }

        renderer_impl::~renderer_impl()
        {
            ui_run_sync(m_window, [this]
            {
                m_mouse.release();
                m_keyboard.release();
            });

            m_resources.wait_for_gpu();
        }

        void renderer_impl::process_user_input()
        {
            m_pad_state         = m_pad.update(m_pad_state);
            m_mouse_state       = m_mouse.update(m_mouse_state);
            m_keyboard_state    = m_keyboard.update(m_keyboard_state);

            io::keyboard_state s = m_keyboard_state;
        }

        void renderer_impl::update()
        {
            m_frame_time = m_frame_timer.seconds();
            m_frame_timer.reset();

            process_user_input();
        }

        void renderer_impl::flush_prerender_queue()
        {
            resize_window_command c;
            while (m_prerender_queue.try_pop(c))
            {
                resize_buffers(&c.m_window_environment);
            }
        }

        void renderer_impl::resize_buffers( const window_environment* environment )
        {
            m_resources.wait_for_gpu();
            m_resources.set_window(environment);
            m_window = environment->m_window;
        }

        void renderer_impl::pre_render()
        {
            //skeleton of a render phases, which will get complicated over time
            flush_prerender_queue();
        }

        void renderer_impl::render()
        {
			using namespace gx;
            using namespace gx::dx12;

            concurrency::task_group g;

            //flush all uploaded resources previous frame
            //make sure the gpu waits for the newly uploaded resources if any
            //flush the previous
            m_resources.direct_queue(device_resources::swap_chains::background)->pix_begin_event(L"Frame");

            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.upload_queue()->flush());
            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.compute_queue()->signal_fence());

            m_resources.direct_queue(device_resources::swap_chains::overlay)->insert_wait_on(m_resources.upload_queue()->flush());
            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.compute_queue()->signal_fence());

            m_frame_index += 1;
            m_frame_index %= 3;

            auto&& back_buffer  = m_resources.back_buffer(device_resources::swap_chains::background);
			auto w = back_buffer->width();
			auto h = back_buffer->height();

			auto&& depth_buffer = m_resources.resource_create_context()->create_frame_depth_buffer(w, h, DXGI_FORMAT_D32_FLOAT);
            auto graphics       = create_graphics_command_context(m_resources.direct_command_context_allocator(device_resources::swap_chains::background));

			//
			pinhole_camera camera;

			camera.set_view_position(math::point3(0, 0, -5));
			pinhole_camera_helper::set_look_at(&camera, math::point3(0, 0, 5), math::vector3(0, 0, -5), math::vector3(0, 1, 0));

			auto perspective = perspective_matrix(camera);
			auto view		 = view_matrix(camera);
			auto world		 = math::identity_matrix();

			interop::frame f;

			f.m_perspective.m_value = transpose(perspective);
			f.m_view.m_value = transpose(view);
			
			graphics->transition_resource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

            graphics->clear(back_buffer);
			graphics->clear(depth_buffer);

			graphics->set_render_target(depth_buffer);
			graphics->set_descriptor_heaps();

			graphics->set_view_port({ 0.0,0.0,static_cast<float>(w),static_cast<float>(h),0.0,1.0 });
			graphics->set_scissor_rectangle({ 0,0,(int32_t)w,(int32_t)(h) });
			graphics->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			//depth
			{
				graphics->set_pso(m_solid_graphics_depth);
				graphics->set_graphics_root_constant(0, 1, offsetof(interop::draw_call, m_batch) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, 0, offsetof(interop::draw_call, m_start_vertex) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_pos, offsetof(interop::draw_call, m_position) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_uv, offsetof(interop::draw_call, m_uv) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_normals, offsetof(interop::draw_call, m_normal) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_tangents, offsetof(interop::draw_call, m_tangent) / sizeof(uint32_t));

				graphics->set_graphics_constant_buffer(1, f);
				graphics->set_graphics_srv_buffer(2, m_geometry.get());

				graphics->set_index_buffer({ m_geometry->virtual_address() + m_mesh.m_indices, m_mesh.m_indices_size, DXGI_FORMAT_R32_UINT });

				{
					auto m = transpose(world);
					graphics->set_graphics_root_constants(0, sizeof(m) / sizeof(uint32_t), &m, offsetof(interop::draw_call, m_world) / sizeof(uint32_t));
				}
				graphics->draw_indexed(m_mesh.m_indices_size / 4);
			}

			graphics->transition_resource(depth_buffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
			//opaque
			{
				graphics->set_render_target(back_buffer, depth_buffer);
				graphics->set_pso(m_solid_graphics);

				graphics->set_view_port({ 0.0,0.0,static_cast<float>(w),static_cast<float>(h),0.0,1.0 });
				graphics->set_scissor_rectangle({ 0,0,(int32_t)w,(int32_t)(h) });
				graphics->set_primitive_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

				graphics->set_graphics_root_constant(0, 1, offsetof(interop::draw_call, m_batch) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, 0, offsetof(interop::draw_call, m_start_vertex) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_pos, offsetof(interop::draw_call, m_position) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_uv, offsetof(interop::draw_call, m_uv) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_normals, offsetof(interop::draw_call, m_normal) / sizeof(uint32_t));
				graphics->set_graphics_root_constant(0, m_mesh.m_tangents, offsetof(interop::draw_call, m_tangent) / sizeof(uint32_t));
				graphics->set_graphics_constant_buffer(1, f);
				graphics->set_graphics_srv_buffer(2, m_geometry.get());

				graphics->set_index_buffer({ m_geometry->virtual_address() + m_mesh.m_indices, m_mesh.m_indices_size, DXGI_FORMAT_R32_UINT });

				{
					auto m = transpose(world);
					graphics->set_graphics_root_constants(0, sizeof(m) / sizeof(uint32_t), &m, offsetof(interop::draw_call, m_world) / sizeof(uint32_t));
				}

				for (auto i = 0U; i < m_mesh_opaque.m_opaque_textures.size(); ++i)
				{
					graphics->set_graphics_dynamic_descriptor(4, m_mesh_opaque.m_opaque_textures[i]->srv());
					graphics->draw_indexed(m_mesh_opaque.m_opaque_ranges[i].index_count(), m_mesh_opaque.m_opaque_ranges[i].m_begin);
				}
			}

			graphics->transition_resource(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

            //if we did upload through the pci bus, insert waits
            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.upload_queue()->flush());
            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.compute_queue()->signal_fence());

            m_resources.direct_queue(device_resources::swap_chains::overlay)->insert_wait_on(m_resources.upload_queue()->flush());
            m_resources.direct_queue(device_resources::swap_chains::background)->insert_wait_on(m_resources.compute_queue()->signal_fence());

			graphics->submit(gpu_command_context::flush_operation::wait_to_execute);
            m_resources.direct_queue(device_resources::swap_chains::background)->pix_end_event();
        }

        void renderer_impl::present()
        {
            m_resources.present();
            m_resources.move_to_next_frame();
            m_resources.sync();
        }

        void renderer_impl::resize()
        {

        }
    }
}