extern "C" {
HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
	::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);

HRESULT __stdcall CreateDirect3D11SurfaceFromDXGISurface(
	::IDXGISurface *dgxiSurface, ::IInspectable **graphicsSurface);
}

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
	IDirect3DDxgiInterfaceAccess : ::IUnknown {
	virtual HRESULT __stdcall GetInterface(GUID const &id,
					       void **object) = 0;
};

extern "C" EXPORT bool winrt_capture_supported()
{
	/* no contract for IGraphicsCaptureItemInterop, verify 10.0.18362.0 */
	return winrt::Windows::Foundation::Metadata::ApiInformation::
		IsApiContractPresent(L"Windows.Foundation.UniversalApiContract",
				     8);
}

template<typename T>
static winrt::com_ptr<T> GetDXGIInterfaceFromObject(
	winrt::Windows::Foundation::IInspectable const &object)
{
	auto access = object.as<IDirect3DDxgiInterfaceAccess>();
	winrt::com_ptr<T> result;
	winrt::check_hresult(
		access->GetInterface(winrt::guid_of<T>(), result.put_void()));
	return result;
}

struct winrt_capture {
	bool capture_cursor;

	gs_texture_t *texture;
	bool texture_written;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{
		nullptr};
	ComPtr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{
		nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{
		nullptr};
	winrt::Windows::Graphics::SizeInt32 last_size;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::
		FrameArrived_revoker frame_arrived;

	bool thread_changed;
	struct winrt_capture *next;

	void on_frame_arrived(winrt::Windows::Graphics::Capture::
				      Direct3D11CaptureFramePool const &sender,
			      winrt::Windows::Foundation::IInspectable const &)
	{
		obs_enter_graphics();

		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame
			frame = sender.TryGetNextFrame();
		const winrt::Windows::Graphics::SizeInt32 frame_content_size =
			frame.ContentSize();

		winrt::com_ptr<ID3D11Texture2D> frame_surface =
			GetDXGIInterfaceFromObject<ID3D11Texture2D>(
				frame.Surface());

		/* need GetDesc because ContentSize is not reliable */
		D3D11_TEXTURE2D_DESC desc;
		frame_surface->GetDesc(&desc);

		if (texture) {
			if (desc.Width != gs_texture_get_width(texture) ||
			    desc.Height != gs_texture_get_height(texture)) {
				gs_texture_destroy(texture);
				texture = nullptr;
			}
		}

		if (!texture) {
			texture = gs_texture_create(desc.Width, desc.Height,
						    GS_BGRA, 1, nullptr, 0);
		}

		/* if they gave an SRV, we could avoid this copy */
		context->CopyResource(
			(ID3D11Texture2D *)gs_texture_get_obj(texture),
			frame_surface.get());

		texture_written = true;

		if (frame_content_size.Width != last_size.Width ||
		    frame_content_size.Height != last_size.Height) {
			frame_pool.Recreate(
				device,
				winrt::Windows::Graphics::DirectX::
					DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2, frame_content_size);

			last_size = frame_content_size;
		}

		obs_leave_graphics();
	}
};

struct winrt_capture *capture_list;

static void winrt_capture_device_loss_release(void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);
	capture->frame_arrived.revoke();
	capture->frame_pool.Close();
	capture->session.Close();

	capture->session = nullptr;
	capture->frame_pool = nullptr;
	capture->context = nullptr;
	capture->device = nullptr;
}

static void winrt_capture_device_loss_rebuild(void *device_void, void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);

	ID3D11Device *const d3d_device = (ID3D11Device *)device_void;
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(d3d_device->QueryInterface(&dxgi_device)))
		blog(LOG_ERROR, "Failed to get DXGI device");

	winrt::com_ptr<IInspectable> inspectable;
	if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(),
							inspectable.put())))
		blog(LOG_ERROR, "Failed to get WinRT device");

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
		device = inspectable.as<winrt::Windows::Graphics::DirectX::
						Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
		frame_pool = winrt::Windows::Graphics::Capture::
			Direct3D11CaptureFramePool::Create(
				device,
				winrt::Windows::Graphics::DirectX::
					DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2, capture->last_size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session =
		frame_pool.CreateCaptureSession(capture->item);

	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->frame_arrived = frame_pool.FrameArrived(
		winrt::auto_revoke,
		{capture, &winrt_capture::on_frame_arrived});

	session.StartCapture();
}

thread_local bool initialized_tls;

extern "C" EXPORT struct winrt_capture *winrt_capture_init(bool cursor,
							   HWND window)
{
	ID3D11Device *const d3d_device = (ID3D11Device *)gs_get_device_obj();
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(d3d_device->QueryInterface(&dxgi_device))) {
		blog(LOG_WARNING, "[winrt_capture_init] Failed to "
				  "get DXGI device");
		return nullptr;
	}

	winrt::com_ptr<IInspectable> inspectable;
	HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(),
							  inspectable.put());
	if (FAILED(hr)) {
		blog(LOG_WARNING, "[winrt_capture_init] Failed to "
				  "get WinRT device");
		return nullptr;
	}

	auto activation_factory = winrt::get_activation_factory<
		winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory =
		activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = {nullptr};
	try {
		interop_factory->CreateForWindow(
			window,
			winrt::guid_of<ABI::Windows::Graphics::Capture::
					       IGraphicsCaptureItem>(),
			reinterpret_cast<void **>(winrt::put_abi(item)));
	} catch (winrt::hresult_invalid_argument &) {
		blog(LOG_WARNING, "[winrt_capture_init] Failed to "
				  "create GraphicsCaptureItem");
		return nullptr;
	}

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
		device = inspectable.as<winrt::Windows::Graphics::DirectX::
						Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::SizeInt32 size = item.Size();
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
		frame_pool = winrt::Windows::Graphics::Capture::
			Direct3D11CaptureFramePool::Create(
				device,
				winrt::Windows::Graphics::DirectX::
					DirectXPixelFormat::B8G8R8A8UIntNormalized,
				2, size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session =
		frame_pool.CreateCaptureSession(item);

	if (capture_list == nullptr)
		initialized_tls = true;

	struct winrt_capture *capture = new winrt_capture{};
	capture->capture_cursor = cursor;
	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->last_size = size;
	capture->frame_arrived = frame_pool.FrameArrived(
		winrt::auto_revoke,
		{capture, &winrt_capture::on_frame_arrived});
	capture->next = capture_list;
	capture_list = capture;

	session.StartCapture();

	gs_device_loss callbacks;
	callbacks.device_loss_release = winrt_capture_device_loss_release;
	callbacks.device_loss_rebuild = winrt_capture_device_loss_rebuild;
	callbacks.data = capture;
	gs_register_loss_callbacks(&callbacks);

	return capture;
}

extern "C" EXPORT void winrt_capture_free(struct winrt_capture *capture)
{
	if (capture) {
		struct winrt_capture *current = capture_list;
		if (current == capture) {
			capture_list = capture->next;
		} else {
			struct winrt_capture *previous;
			do {
				previous = current;
				current = current->next;
			} while (current != capture);

			previous->next = current->next;
		}

		obs_enter_graphics();
		gs_unregister_loss_callbacks(capture);
		gs_texture_destroy(capture->texture);
		obs_leave_graphics();

		capture->frame_arrived.revoke();
		capture->frame_pool.Close();
		capture->session.Close();

		delete capture;
	}
}

static void draw_texture(struct winrt_capture *capture, gs_effect_t *effect)
{
	gs_texture_t *const texture = capture->texture;
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	size_t passes;

	gs_effect_set_texture(image, texture);

	passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		if (gs_technique_begin_pass(tech, i)) {
			gs_draw_sprite(texture, 0, 0, 0);

			gs_technique_end_pass(tech);
		}
	}
	gs_technique_end(tech);
}

extern "C" EXPORT void winrt_capture_render(struct winrt_capture *capture,
					    gs_effect_t *effect)
{
	if (capture && capture->texture_written) {
		if (!initialized_tls) {
			struct winrt_capture *current = capture_list;
			while (current) {
				current->thread_changed = true;
				current = current->next;
			}

			initialized_tls = true;
		}

		if (capture->thread_changed) {
			/* new graphics thread. treat like device loss. */
			winrt_capture_device_loss_release(capture);
			winrt_capture_device_loss_rebuild(gs_get_device_obj(),
							  capture);

			capture->thread_changed = false;
		}

		draw_texture(capture, effect);
	}
}

extern "C" EXPORT int32_t
winrt_capture_width(const struct winrt_capture *capture)
{
	return capture ? capture->last_size.Width : 0;
}

extern "C" EXPORT int32_t
winrt_capture_height(const struct winrt_capture *capture)
{
	return capture ? capture->last_size.Height : 0;
}
