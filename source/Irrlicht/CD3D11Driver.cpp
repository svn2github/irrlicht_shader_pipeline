// Copyright (C) 2002-2009 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
// 
#define _IRR_DONT_DO_MEMORY_DEBUGGING_HERE
#include "CD3D11Driver.h"

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "os.h"
#include "S3DVertex.h"
#include "CD3D11Texture.h"
#include "CD3D11HardwareBuffer.h"
#include "CImage.h"
#include "CD3D11MaterialRenderer.h"
#include "CD3D11FixedFunctionMaterialRenderer.h"
#include "CD3D11CustomMaterialRenderer.h"
#include "CD3D11NormalMapRenderer.h"
#include "CD3D11VertexDeclaration.h"
//#include "CD3D11ParallaxMapRenderer.h"

#define SAFE_RELEASE(x) \
	if(x)				\
		x->Release(); 	\
	x = 0;

#define RELEASE_ALL(x)			\
	ULONG rest = x->Release();	\
	while( rest > 0 )			\
		rest = x->Release();	\
	x = 0;


inline irr::u32 getIndexSize(irr::video::E_INDEX_TYPE iType)
{
	return (iType == irr::video::EIT_16BIT) ? sizeof(unsigned short) : sizeof(unsigned int);
};

inline void unpack_texureBlendFunc ( irr::video::E_BLEND_FACTOR &srcFact, irr::video::E_BLEND_FACTOR &dstFact,
			irr::video::E_MODULATE_FUNC &modulo, irr::u32& alphaSource, const irr::f32 param )
	{
		const irr::u32 state = IR(param);
		alphaSource = (state & 0x0000F000) >> 12;
		modulo	= irr::video::E_MODULATE_FUNC( ( state & 0x00000F00 ) >> 8 );
		srcFact = irr::video::E_BLEND_FACTOR ( ( state & 0x000000F0 ) >> 4 );
		dstFact = irr::video::E_BLEND_FACTOR ( ( state & 0x0000000F ) );
	}


namespace irr
{

namespace video
{

//! constructor
CD3D11Driver::CD3D11Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
: 	CNullDriver(io, params.WindowSize),
	Params(params),
	ResetRenderStates(true), Transformation3DChanged(false),
	DriverType(D3D_DRIVER_TYPE_HARDWARE),
	D3DLibrary(0), Device(0), ImmediateContext(0), SwapChain(0), Adapter(0), DXGIFactory(0), 
	Output(0), CurrentInputLayout(0), DefaultBackBuffer(0), DefaultDepthBuffer(0),
	DynVertexBuffer(0), DynIndexBuffer(0), DynVertexBufferSize(0), DynIndexBufferSize(0),
	SceneSourceRect(0), VendorID(0), ColorFormat(ECF_A8R8G8B8),
	CurrentRenderMode(ERM_NONE), MaxActiveLights(8), AlphaToCoverageSupport(true),
	DepthStencilFormat(DXGI_FORMAT_UNKNOWN), D3DColorFormat(DXGI_FORMAT_R8G8B8A8_UNORM),
	LastVertexType((video::E_VERTEX_TYPE)-1),
	NullTexture(0), MaxTextureUnits(MATERIAL_MAX_TEXTURES) // DirectX 11 can handle much more than this value, but keep compatibility

{
	#ifdef _DEBUG
	setDebugName("CD3D11Driver");
	#endif

	printVersion();

	for(u32 i=0; i<MATERIAL_MAX_TEXTURES; ++i)
	{
		CurrentTexture[i] = 0;
	}

	// init clip planes
	ClipPlanes.push_back( core::plane3df() );
	ClipPlanes.push_back( core::plane3df() );
	ClipPlanes.push_back( core::plane3df() );
	ClipPlaneEnabled[0] = ClipPlaneEnabled[1] = ClipPlaneEnabled[2] = false;

	// create sphere map matrix
	SphereMapMatrixD3D11._11 = 0.5f; SphereMapMatrixD3D11._12 = 0.0f;
	SphereMapMatrixD3D11._13 = 0.0f; SphereMapMatrixD3D11._14 = 0.0f;
	SphereMapMatrixD3D11._21 = 0.0f; SphereMapMatrixD3D11._22 =-0.5f;
	SphereMapMatrixD3D11._23 = 0.0f; SphereMapMatrixD3D11._24 = 0.0f;
	SphereMapMatrixD3D11._31 = 0.0f; SphereMapMatrixD3D11._32 = 0.0f;
	SphereMapMatrixD3D11._33 = 1.0f; SphereMapMatrixD3D11._34 = 0.0f;
	SphereMapMatrixD3D11._41 = 0.5f; SphereMapMatrixD3D11._42 = 0.5f;
	SphereMapMatrixD3D11._43 = 0.0f; SphereMapMatrixD3D11._44 = 1.0f;

	core::matrix4 mat;
	UnitMatrixD3D11 = *(D3DMATRIX*)((void*)mat.pointer());
}

CD3D11Driver::~CD3D11Driver()
{
	// Unbound all shader resources
	ID3D11ShaderResourceView* views[1] = { NULL };
	ImmediateContext->VSSetShaderResources(0, 1, views);
	ImmediateContext->HSSetShaderResources(0, 1, views);
	ImmediateContext->DSSetShaderResources(0, 1, views);
	ImmediateContext->GSSetShaderResources(0, 1, views);
	ImmediateContext->PSSetShaderResources(0, 1, views);
	ImmediateContext->CSSetShaderResources(0, 1, views);

	// Set windowed mode before release swap chain
	SwapChain->SetFullscreenState( FALSE, NULL );

	NullTexture->drop();
	// Delete renderers and textures
	deleteMaterialRenders();
	deleteAllTextures();
	removeAllOcclusionQueries();
	removeAllHardwareBuffers();

	ClipPlanes.clear();

	// clear state
	if (ImmediateContext)
	{
		ImmediateContext->ClearState();
		ImmediateContext->Flush();
	}

	// release Direct3D objects

	// clear vertex declarations
	DeclarationIterator it = declarationMap.getIterator();
	while(!it.atEnd())
	{
		it->getValue()->drop();
		it++;
	}
	declarationMap.clear();

	// release blend states
	BlendStateMap::Iterator bldIt = BlendMap.getIterator();
	while (!bldIt.atEnd())
	{
		if (bldIt->getValue()) bldIt->getValue()->Release();
		bldIt++;
	}
	BlendMap.clear();

	// release rasterizer states
	RasterizerStateMap::Iterator rasIt = RasterizerMap.getIterator();
	while (!rasIt.atEnd())
	{
		if (rasIt->getValue()) rasIt->getValue()->Release();
		rasIt++;
	}
	RasterizerMap.clear();
	
	// release depth stencil states
	DepthStencilStateMap::Iterator dsIt = DepthStencilMap.getIterator();
	while (!dsIt.atEnd())
	{
		if (dsIt->getValue()) dsIt->getValue()->Release();
		dsIt++;
	}
	DepthStencilMap.clear();
	
	// release sampler states
	SamplerStateMap::Iterator samIt = SamplerMap.getIterator();
	while (!samIt.atEnd())
	{
		if (samIt->getValue()) samIt->getValue()->Release();
		samIt++;
	}
	SamplerMap.clear();

	SAFE_RELEASE(DynIndexBuffer);
	SAFE_RELEASE(DynVertexBuffer);
	SAFE_RELEASE(SwapChain);
	SAFE_RELEASE(DefaultDepthBuffer);
	SAFE_RELEASE(DefaultBackBuffer);
	SAFE_RELEASE(Output);
	SAFE_RELEASE(Adapter);
	SAFE_RELEASE(ImmediateContext);
	SAFE_RELEASE(DXGIFactory);
	RELEASE_ALL(Device);

	FreeLibrary(D3DLibrary);
}

void CD3D11Driver::createMaterialRenderers()
{
	// create D3D11 material renderers
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_SOLID(Device, this) );
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_SOLID_2_LAYER(Device, this) ); // video::EMT_SOLID_2_LAYER;

	// add the same renderer for all lightmap types
	CD3D11MaterialRenderer_LIGHTMAP* lmr = new CD3D11MaterialRenderer_LIGHTMAP(Device, this);
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_ADD
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_M2
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_M4
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_LIGHTING
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_LIGHTING_M2
	addMaterialRenderer( lmr ); // video::EMT_LIGHTMAP_LIGHTING_M4
	lmr->drop();

	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_DETAIL_MAP(Device, this) ); // video::EMT_DETAIL_MAP
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_SPHERE_MAP(Device, this) ); // video::EMT_SPHERE_MAP
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_REFLECTION_2_LAYER(Device, this) ); // video::EMT_REFLECTION_2_LAYER
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_TRANSPARENT_ADD_COLOR(Device, this) ); // video::EMT_TRANSPARENT_ADD_COLOR
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL(Device, this) ); // video::EMT_TRANSPARENT_ALPHA_CHANNEL
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL_REF(Device, this) ); // video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_TRANSPARENT_VERTEX_ALPHA(Device, this) ); // video::EMT_TRANSPARENT_VERTEX_ALPHA
	addAndDropMaterialRenderer( new CD3D11MaterialRenderer_TRANSPARENT_REFLECTION_2_LAYER(Device, this) ); // video::EMT_TRANSPARENT_REFLECTION_2_LAYER

	// add normal map renderers
	s32 tmp = 0;
	video::IMaterialRenderer* renderer = 0;

	renderer = new CD3D11NormalMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_SOLID].Renderer);
	renderer->drop();

	renderer = new CD3D11NormalMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_TRANSPARENT_ADD_COLOR].Renderer);
	renderer->drop();

	renderer = new CD3D11NormalMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_TRANSPARENT_VERTEX_ALPHA].Renderer);
	renderer->drop();
	
	// The following shall be changed to parallax in future
/*	renderer = new CD3D11ParallaxMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_SOLID].Renderer);
	renderer->drop();

	renderer = new CD3D11ParallaxMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_TRANSPARENT_ADD_COLOR].Renderer);
	renderer->drop();

	renderer = new CD3D11ParallaxMapRenderer(Device, this, tmp,
		MaterialRenderers[EMT_TRANSPARENT_VERTEX_ALPHA].Renderer);
	renderer->drop();*/

	// The following shall be changed to one blend in future
	addAndDropMaterialRenderer(new CD3D11MaterialRenderer_ONETEXTURE_BLEND(Device, this));
}

//! initialises the Direct3D API
bool CD3D11Driver::initDriver(HWND hwnd, bool pureSoftware)
{
	HRESULT hr;

	if(!Device)
	{
		D3DLibrary = LoadLibrary( __TEXT("d3d11.dll") );
		if (!D3DLibrary)
		{
			os::Printer::log("Error, could not load d3d11.dll.", ELL_ERROR);
			return false;
		}

		// Get function address
		PFN_D3D11_CREATE_DEVICE CreateDeviceFunc = (PFN_D3D11_CREATE_DEVICE) GetProcAddress( D3DLibrary, "D3D11CreateDevice" );
		if( !CreateDeviceFunc )
		{
			os::Printer::log("Error, could not get D3D11CreateDevice function.", ELL_ERROR);
			return false;
		}

		// Device flags
		UINT deviceFlags = 0;

#ifdef _DEBUG
		deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		if(!Params.DriverMultithreaded)
			deviceFlags |= D3D11_CREATE_DEVICE_SINGLETHREADED;

		// a specific video card?
		if(Params.DisplayAdapter)
		{
			FARPROC pfnFunc = GetProcAddress( D3DLibrary, "CreateDXGIFactory" );
			if( !pfnFunc )
			{
				os::Printer::log("Error, could not get CreateDXGIFactory function.", ELL_ERROR);
				return false;
			}

			typedef HRESULT (__stdcall * pICFUNC)(const IID&, void**);

			pICFUNC CreateDXGIFactoryFunc;
			CreateDXGIFactoryFunc = pICFUNC(pfnFunc); 

			IDXGIFactory * pFactory;
			HRESULT hResult = CreateDXGIFactoryFunc(__uuidof(IDXGIFactory), (void**)(&pFactory));
			if(FAILED(hResult))
			{
				os::Printer::log("Error, could not create factory.", ELL_ERROR);
				return false;
			}

			if(pFactory->EnumAdapters(Params.DisplayAdapter, &Adapter) == DXGI_ERROR_NOT_FOUND) 
			{
				pFactory->Release();
				os::Printer::log("Error, could not found display adapter.", ELL_ERROR);
				return false;
			}

			pFactory->Release();
		}

		if(pureSoftware)
			DriverType = D3D_DRIVER_TYPE_SOFTWARE;

		// Try creating hardware device
		D3D_FEATURE_LEVEL RequestedLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		u32 RequestedLevelsSize = sizeof( RequestedLevels ) / sizeof( RequestedLevels[0] );

		hr = CreateDeviceFunc(Adapter, DriverType, NULL, deviceFlags, RequestedLevels, RequestedLevelsSize, D3D11_SDK_VERSION, &Device, &FeatureLevel, &ImmediateContext );
		if (FAILED(hr))
		{
			// Try creating warp device and feature level 10.1
			DriverType = D3D_DRIVER_TYPE_WARP;
			hr = CreateDeviceFunc(Adapter, DriverType, NULL, deviceFlags, RequestedLevels, RequestedLevelsSize, D3D11_SDK_VERSION, &Device, &FeatureLevel, &ImmediateContext );
			if(FAILED(hr))
			{
				os::Printer::log("Error, could not create device.", ELL_ERROR);
				return false;
			}
		}

		// Printing type of Direct3D 11 device and feature level
		char msg[512];
		sprintf(msg, "Using %s device, feature level %s",
					 (DriverType == D3D_DRIVER_TYPE_HARDWARE) ? "Hardware" : "WARP",
						(FeatureLevel == D3D_FEATURE_LEVEL_11_0) ? "11.0" :
						(FeatureLevel == D3D_FEATURE_LEVEL_10_1) ? "10.1" : "10.0");
		os::Printer::log(msg, ELL_INFORMATION);

	}

	// Get adapter used by this device and query informations
	IDXGIDevice* DXGIDevice = NULL;
	Device->QueryInterface( __uuidof( IDXGIDevice ), reinterpret_cast<void**>( &DXGIDevice ) );
	if( SUCCEEDED( DXGIDevice->GetAdapter( &Adapter ) ) )
	{
		DXGI_ADAPTER_DESC adapDesc;
		::ZeroMemory( &adapDesc, sizeof( DXGI_ADAPTER_DESC ) );

		Adapter->GetDesc( &adapDesc );
		
		s32 Product = HIWORD(adapDesc.AdapterLuid.HighPart);
		s32 Version = LOWORD(adapDesc.AdapterLuid.HighPart);
		s32 SubVersion = HIWORD(adapDesc.AdapterLuid.LowPart);
		s32 Build = LOWORD(adapDesc.AdapterLuid.LowPart);

		wchar_t tmp[512];
		swprintf(tmp, L"%s %d.%d.%d.%d", adapDesc.Description, Product, Version, SubVersion, Build);
		os::Printer::log(tmp, ELL_INFORMATION);

		// Assign vendor name based on vendor id.
		VendorID= static_cast<u16>(adapDesc.VendorId);
		switch(VendorID)
		{
			case 0x1002 : VendorName = "ATI Technologies Inc."; break;
			case 0x10DE : VendorName = "NVIDIA Corporation"; break;
			case 0x102B : VendorName = "Matrox Electronic Systems Ltd."; break;
			case 0x121A : VendorName = "3dfx Interactive Inc"; break;
			case 0x5333 : VendorName = "S3 Graphics Co., Ltd."; break;
			case 0x8086 : VendorName = "Intel Corporation"; break;
			default: VendorName = "Unknown VendorId: "; VendorName += (u32)adapDesc.VendorId; break;
		}
	}

	// get DXGI factory
	hr = Adapter->GetParent( __uuidof( IDXGIFactory ), reinterpret_cast<void**>( &DXGIFactory ) );
	if( FAILED( hr ) )
	{
		os::Printer::log("Error, could not get DXGI factory.", ELL_ERROR);
		return false;
	}

	// get DXGI output used (the monitor)
	hr = Adapter->EnumOutputs( 0, &Output );
	if( FAILED( hr ) )
	{
		os::Printer::log("Error, could not get monitor", ELL_INFORMATION);
	}

	// Preparing for swap chain creation
	::ZeroMemory( &present, sizeof( DXGI_SWAP_CHAIN_DESC ) );
	present.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	present.BufferCount = 1;
	present.BufferDesc.Format = D3DColorFormat;
	present.BufferDesc.Width = Params.WindowSize.Width;
	present.BufferDesc.Height = Params.WindowSize.Height;
	present.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	//present.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
	present.SampleDesc.Count = 1;
	present.SampleDesc.Quality = 0;
	present.OutputWindow = (HWND)Params.WindowId;

	if (Params.Vsync)
		present.BufferDesc.RefreshRate.Numerator = 60;
	else
		present.BufferDesc.RefreshRate.Numerator = 0;

	present.BufferDesc.RefreshRate.Denominator = 1;
	present.Windowed = !Params.Fullscreen;	

	// Adjust for full screen
	if(Params.Fullscreen)
	{	
		present.BufferDesc.Scaling = DXGI_MODE_SCALING_STRETCHED;
		present.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST;
		present.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH ;

		// Optimization: query a display mode closest to requested.
		// This will make surface flip, instead of blit
		if(Output)
		{
			DXGI_MODE_DESC* request = &(present.BufferDesc);
			DXGI_MODE_DESC match;
			ZeroMemory( &match, sizeof( DXGI_MODE_DESC ) );
			hr = Output->FindClosestMatchingMode( request, &match, Device );
			if( SUCCEEDED(hr) ) 
			{
				present.BufferDesc.Format = match.Format;
				present.BufferDesc.Height = match.Height;
				present.BufferDesc.RefreshRate = match.RefreshRate;
				present.BufferDesc.Scaling = match.Scaling;
				present.BufferDesc.ScanlineOrdering = match.ScanlineOrdering;
				present.BufferDesc.Width = match.Width;

				// Adjust screen size accordingly
				ScreenSize.Width = match.Width;
				ScreenSize.Height = match.Height;
				Params.WindowSize = ScreenSize;
			}
		}
	}

	// check anti alias, ONLY if driver is pure hardware (just for performance reasons)
	if( Params.AntiAlias > 0 && DriverType != D3D_DRIVER_TYPE_WARP )
	{
		if (Params.AntiAlias > 32)
			Params.AntiAlias = 32;

		UINT qualityLevels = 0;

		while(Params.AntiAlias > 0)
		{
			if(SUCCEEDED(Device->CheckMultisampleQualityLevels(present.BufferDesc.Format, Params.AntiAlias, &qualityLevels)))
			{
				present.SampleDesc.Count = Params.AntiAlias;
				present.SampleDesc.Quality = qualityLevels-1;
				present.SwapEffect	 = DXGI_SWAP_EFFECT_DISCARD;
				break;
			}
			--Params.AntiAlias;
		}

		if (Params.AntiAlias==0)
		{
			os::Printer::log("Anti aliasing disabled because hardware/driver lacks necessary caps.", ELL_WARNING);
		}
	}

	// Create swap chain
	hr = DXGIFactory->CreateSwapChain( DXGIDevice, &present, &SwapChain );
	if( FAILED(hr))
	{
		// Try a second time, may fail the first time due to back buffer count
		hr = DXGIFactory->CreateSwapChain( DXGIDevice, &present, &SwapChain );
		if(FAILED(hr))
		{
			os::Printer::log("Error, could not create swap chain.", ELL_ERROR);
			return false;
		}
	}
	DXGIDevice->Release();

	// Get default render target
	ID3D11Texture2D* backBuffer = NULL;
	hr = SwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast<void**>( &backBuffer ) );
	if( FAILED(hr))
	{
		os::Printer::log("Error, could not get back buffer.", ELL_ERROR);
		return false;
	}

	hr = Device->CreateRenderTargetView( backBuffer, NULL, &DefaultBackBuffer );
	if( FAILED(hr))
	{
		os::Printer::log("Error, could not create render target view.", ELL_ERROR);
		return false;
	}
	backBuffer->Release();

	// creating depth buffer
	DefaultDepthBuffer = createDepthStencilView(ScreenSize);

	// set exposed data
	ExposedData.D3D11.D3DDev11 = Device;
	ExposedData.D3D11.SwapChain = SwapChain;
	ExposedData.D3D11.HWnd = hwnd;

	// register vertex types
	core::array<SVertexElement> el;
	el.push_back(SVertexElement(EVES_POSITION, EVET_FLOAT3, 0));
	el.push_back(SVertexElement(EVES_NORMAL, EVET_FLOAT3, 0));
	el.push_back(SVertexElement(EVES_COLOR, EVET_COLOUR, 0));
	el.push_back(SVertexElement(EVES_TEXTURE_COORD, EVET_FLOAT2, 0));
	registerVertexType(el);

	el.push_back(SVertexElement(EVES_TEXTURE_COORD, EVET_FLOAT2, 1));
	registerVertexType(el);

	el[4] = SVertexElement(EVES_TEXTURE_COORD, EVET_FLOAT3, 1);
	el.push_back(SVertexElement(EVES_TEXTURE_COORD, EVET_FLOAT3, 2));
	registerVertexType(el);

	// init states description
	BlendDesc.reset();
	RasterizerDesc.reset();
	DepthStencilDesc.reset();
	for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; i++)
		SamplerDesc[i].reset();

	// Init dynamic buffers
	reallocateDynamicBuffers( 32768 * sizeof(S3DVertexTangents), 32768 * sizeof(int) );

	// With all DX 11 objects created, init driver states

	// Only enable multisampling if needed
	disableFeature(EVDF_TEXTURE_MULTISAMPLING, true);

	// Create a texture that will be used when material textures are NULL
	// 1x1 texture, opaque White.
	u32 data = 0xffffffff;
	IImage* image = this->createImageFromData( ECF_A8R8G8B8, core::dimension2du(1, 1), &data, false);
	NullTexture = static_cast<CD3D11Texture*>(this->addTexture( "NullTexture", image ));	// DO NOT DROP THIS POINTER (only drop with getTexture)
	image->drop();
	NullTexture->grab();

	// Set render targets
	setRenderTarget(0, true, true);

	LastVertexType = EVT_STANDARD;

	// set fog mode
	setFog(FogColor, EFT_FOG_EXP, FogStart, FogEnd, FogDensity, PixelFog, RangeFog);

	ResetRenderStates = true;

	// create materials
	createMaterialRenderers();

	DriverAttributes->setAttribute("MaxTextures", (s32)MaxTextureUnits);
	DriverAttributes->setAttribute("MaxSupportedTextures", (s32)MaxTextureUnits);
	DriverAttributes->setAttribute("MaxLights", (s32)MaxActiveLights);
	DriverAttributes->setAttribute("MaxAnisotropy", (s32)D3D11_MAX_MAXANISOTROPY);
	DriverAttributes->setAttribute("MaxUserClipPlanes", (s32)MaxUserClipPlanes);
	DriverAttributes->setAttribute("MaxMultipleRenderTargets", D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
	DriverAttributes->setAttribute("MaxIndices", (s32)getMaximalPrimitiveCount());
	DriverAttributes->setAttribute("MaxTextureSize", (s32)getMaxTextureSize().Width);
	DriverAttributes->setAttribute("MaxTextureLODBias", (s32)SamplerDesc->MipLODBias);
	DriverAttributes->setAttribute("Version", Params.DriverType == EDT_DIRECT3D11 ? 110 : 101);
	DriverAttributes->setAttribute("ShaderLanguageVersion", Params.DriverType == EDT_DIRECT3D11 ? 50 : 40);
	DriverAttributes->setAttribute("AntiAlias", Params.AntiAlias);

	// clear textures
	for(u32 i = 0; i < MATERIAL_MAX_TEXTURES; i++)
		setActiveTexture(i, 0);

	return true;
}

//! Create occlusion query.
/** Use node for identification and mesh for occlusion test. */
void CD3D11Driver::addOcclusionQuery(scene::ISceneNode* node, const scene::IMesh* mesh)
{
	if (!queryFeature(EVDF_OCCLUSION_QUERY))
		return;

	CNullDriver::addOcclusionQuery(node, mesh);

	const s32 index = OcclusionQueries.linear_search(SOccQuery(node));
	if ((index != -1) && (OcclusionQueries[index].PID == 0))
	{
		D3D11_QUERY_DESC desc;
		desc.Query = D3D11_QUERY_OCCLUSION;
		desc.MiscFlags = 0;
		Device->CreateQuery(&desc, reinterpret_cast<ID3D11Query**>(&OcclusionQueries[index].PID));
	}
}

//! Return query result.
/** Return value is the number of visible pixels/fragments.
The value is a safe approximation, i.e. can be larger than the
actual value of pixels. */
u32 CD3D11Driver::getOcclusionQueryResult(scene::ISceneNode* node) const
{
	const s32 index = OcclusionQueries.linear_search(SOccQuery(node));
	if (index != -1)
		return OcclusionQueries[index].Result;
	else
		return ~0;
}

//! Remove occlusion query.
void CD3D11Driver::removeOcclusionQuery(scene::ISceneNode* node)
{
	const s32 index = OcclusionQueries.linear_search(SOccQuery(node));
	if (index != -1)
	{
		if (OcclusionQueries[index].PID != 0)
			reinterpret_cast<ID3D11Query*>(OcclusionQueries[index].PID)->Release();
		CNullDriver::removeOcclusionQuery(node);
	}
}

//! Run occlusion query. Draws mesh stored in query.
/** If the mesh shall not be rendered visible, use
overrideMaterial to disable the color and depth buffer. */
void CD3D11Driver::runOcclusionQuery(scene::ISceneNode* node, bool visible)
{
	if (!node)
		return;

	const s32 index = OcclusionQueries.linear_search(SOccQuery(node));
	if (index != -1)
	{
		if (OcclusionQueries[index].PID)
			ImmediateContext->Begin(reinterpret_cast<ID3D11Query*>(OcclusionQueries[index].PID));
		
		CNullDriver::runOcclusionQuery(node, visible);
		
		if (OcclusionQueries[index].PID)
			ImmediateContext->End(reinterpret_cast<ID3D11Query*>(OcclusionQueries[index].PID));
	}
}

//! Update occlusion query. Retrieves results from GPU.
/** If the query shall not block, set the flag to false.
Update might not occur in this case, though */
void CD3D11Driver::updateOcclusionQuery(scene::ISceneNode* node, bool block)
{
	const s32 index = OcclusionQueries.linear_search(SOccQuery(node));
	if (index != -1)
	{
		// not yet started
		if (OcclusionQueries[index].Run==u32(~0))
			return;

		bool available = block;
		GUID tmp;
		u32 size = sizeof(DWORD);
		u32 r = 0;

		if (!block)
			available=(reinterpret_cast<ID3D11Query*>(OcclusionQueries[index].PID)->GetPrivateData(tmp, &size, &r) == S_OK);
		else
		{
			do
			{
				HRESULT hr = reinterpret_cast<ID3D11Query*>(OcclusionQueries[index].PID)->GetPrivateData(tmp, &size, &r);
				available = (hr == S_OK);
				if (hr!=S_FALSE)
					break;
			} while (!available);
		}
		if (available)
			OcclusionQueries[index].Result = r;
	}
}

ID3D11BlendState* CD3D11Driver::getBlendState()
{
	// Blend state
	ID3D11BlendState* state = 0;
	BlendStateMap::Node* bldIt = BlendMap.find( BlendDesc );
	if (bldIt)
	{
		state = bldIt->getValue();
	}
	else	// if not found, create and insert into map
	{
		if (SUCCEEDED(Device->CreateBlendState( &BlendDesc, &state )))
		{
			BlendMap.insert( BlendDesc, state );
		}
		else
		{
			os::Printer::log("Error, failed to create blend state", ELL_ERROR);
		}
	}

	return state;
}

ID3D11RasterizerState* CD3D11Driver::getRasterizerState()
{
	// Rasterizer state
	ID3D11RasterizerState* state = 0;
	RasterizerStateMap::Node* rasIt = RasterizerMap.find( RasterizerDesc );
	if (rasIt)
	{
		state = rasIt->getValue();
	}
	else	// if not found, create and insert into map
	{
		if (SUCCEEDED(Device->CreateRasterizerState( &RasterizerDesc, &state )))
		{
			RasterizerMap.insert( RasterizerDesc, state );
		}
		else
		{
			os::Printer::log("Error, failed to create rasterizer state", ELL_ERROR);
		}
	}

	return state;
}

ID3D11DepthStencilState* CD3D11Driver::getDepthStencilState()
{
	// Depth stencil state
	ID3D11DepthStencilState* state = NULL;
	DepthStencilStateMap::Node* dsIt = DepthStencilMap.find( DepthStencilDesc );
	if (dsIt)
	{
		state = dsIt->getValue();
	}
	else	// if not found, create and insert into map
	{
		if (SUCCEEDED(Device->CreateDepthStencilState( &DepthStencilDesc, &state )))
		{
			DepthStencilMap.insert( DepthStencilDesc, state);
		}
		else
		{
			os::Printer::log("Error, failed to create depth stencil state", ELL_ERROR);
		}
	}

	return state;
}

ID3D11SamplerState* CD3D11Driver::getSamplerState(u32 idx)
{
	// Depth stencil state
	ID3D11SamplerState* state = 0;
	SamplerStateMap::Node* samIt = SamplerMap.find( SamplerDesc[idx] );
	if (samIt)
	{
		state = samIt->getValue();
	}
	else	// if not found, create and insert into map
	{
		if (SUCCEEDED(Device->CreateSamplerState( &SamplerDesc[idx], &state )))
		{
			SamplerMap.insert( SamplerDesc[idx], state );
		}
		else
		{
			os::Printer::log("Error, failed to create sampler state", ELL_ERROR);
		}
	}

	return state;
}

//! applications must call this method before performing any rendering. returns false if failed.
bool CD3D11Driver::beginScene(bool backBuffer, bool zBuffer, SColor color,
							  const SExposedVideoData& videoData, core::rect<s32>* sourceRect)
{
	CNullDriver::beginScene(backBuffer, zBuffer, color, videoData, sourceRect);

	if( backBuffer )
	{
		SColorf fCol( color );
		FLOAT c[4] = { fCol.r, fCol.g, fCol.b, fCol.a };		// don't swizzle clear color for default back buffer
		ImmediateContext->ClearRenderTargetView( DefaultBackBuffer, c );
	}
	if( zBuffer && DefaultDepthBuffer )
	{
		ImmediateContext->ClearDepthStencilView( DefaultDepthBuffer, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0 );
	}

	// For safety, unset all shader resource views
	// If some is bound to shader stages, can cause error when bound to Output Merger too
	ID3D11ShaderResourceView* views[1] = { NULL };
	ImmediateContext->VSSetShaderResources( 0, 1, views );
	ImmediateContext->GSSetShaderResources( 0, 1, views );
	ImmediateContext->PSSetShaderResources( 0, 1, views );

	return true;
}

//! applications must call this method after performing any rendering. returns false if failed.
bool CD3D11Driver::endScene()
{
	CNullDriver::endScene();

	HRESULT hr = S_OK;
	
	hr = SwapChain->Present( Params.Vsync ? 1 : 0, 0 );
	
	if( FAILED( hr ) )
	{
		os::Printer::log("Error, could not present frame to screen", ELL_ERROR);
		return false;
	}

	return true;
}

bool CD3D11Driver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
{
	if (!FeatureEnabled[feature])
		return false;

	switch( feature )
	{
	case EVDF_MULTIPLE_RENDER_TARGETS:
		return DriverType == D3D_DRIVER_TYPE_HARDWARE;

	case EVDF_MRT_BLEND:
	case EVDF_ALPHA_TO_COVERAGE:
	case EVDF_MRT_BLEND_FUNC:	
	case EVDF_TEXTURE_NSQUARE:
	case EVDF_VERTEX_BUFFER_OBJECT:
	case EVDF_COLOR_MASK:
	case EVDF_RENDER_TO_TARGET:
	case EVDF_MULTITEXTURE:
	case EVDF_BILINEAR_FILTER:
	case EVDF_TEXTURE_NPOT:
	case EVDF_STENCIL_BUFFER:
	case EVDF_HLSL:
	case EVDF_MIP_MAP:
	case EVDF_MIP_MAP_AUTO_UPDATE:
	case EVDF_TEXTURE_MULTISAMPLING:
	case EVDF_VERTEX_SHADER_1_1:
	case EVDF_VERTEX_SHADER_2_0:
	case EVDF_VERTEX_SHADER_3_0:
	case EVDF_PIXEL_SHADER_1_1:
	case EVDF_PIXEL_SHADER_1_2:
	case EVDF_PIXEL_SHADER_1_3:
	case EVDF_PIXEL_SHADER_1_4:
	case EVDF_PIXEL_SHADER_2_0:
	case EVDF_PIXEL_SHADER_3_0:
		return true;

	case EVDF_VERTEX_SHADER_4_0:
	case EVDF_PIXEL_SHADER_4_0:
	case EVDF_GEOMETRY_SHADER_4_0:
	case EVDF_STREAM_OUTPUT_4_0:
		return FeatureLevel == D3D_FEATURE_LEVEL_10_0 ||
				FeatureLevel == D3D_FEATURE_LEVEL_10_1 ||
				FeatureLevel == D3D_FEATURE_LEVEL_11_0;

	case EVDF_VERTEX_SHADER_4_1:
	case EVDF_PIXEL_SHADER_4_1:
	case EVDF_GEOMETRY_SHADER_4_1:
	case EVDF_STREAM_OUTPUT_4_1:
		return FeatureLevel == D3D_FEATURE_LEVEL_10_1 ||
			   FeatureLevel == D3D_FEATURE_LEVEL_11_0;

	case EVDF_VERTEX_SHADER_5_0:
	case EVDF_PIXEL_SHADER_5_0:
	case EVDF_GEOMETRY_SHADER_5_0:
	case EVDF_STREAM_OUTPUT_5_0:
	case EVDF_TESSELATION_SHADER_5_0:
		return FeatureLevel == D3D_FEATURE_LEVEL_11_0;
	
	case EVDF_COMPUTING_SHADER_4_0:
	case EVDF_COMPUTING_SHADER_4_1:
		{
			D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS opts;
			if (SUCCEEDED( Device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, 
						&opts, sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS)) ))
			{
				return opts.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x != 0;
			}
			return false;
		}

	case EVDF_COMPUTING_SHADER_5_0:
		return FeatureLevel == D3D_FEATURE_LEVEL_11_0;
	}

	return false;
}

//! sets transformation
void CD3D11Driver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
{
	Transformation3DChanged = true;
	Matrices[state] = mat;
}

bool CD3D11Driver::setActiveTexture(u32 stage, const video::ITexture* texture)
{
	if (texture && texture->getDriverType() != EDT_DIRECT3D11)
	{
		os::Printer::log("Fatal Error: Tried to set a texture not owned by this driver.", ELL_ERROR);
		return false;
	}

	// For 1st, 2nd stage, set NullTexture if texture is NULL
	CurrentTexture[stage] = ((stage == 0 || stage == 1 ) && texture == 0) ? NullTexture : texture;

	return true;
}

//! sets a material
void CD3D11Driver::setMaterial(const SMaterial& material)
{
	Material = material;
	OverrideMaterial.apply(Material);

	// set textures
	for(u16 i = 0; i < MATERIAL_MAX_TEXTURES; i++)
	{	
		setActiveTexture(i, Material.getTexture(i));
		setTransform((E_TRANSFORMATION_STATE) ( ETS_TEXTURE_0 + i ), Material.getTextureMatrix(i));
	}
}

bool CD3D11Driver::setRenderTarget(video::ITexture* texture,
			bool clearBackBuffer, bool clearZBuffer, SColor color)
{
	// check for right driver type
	if (texture && texture->getDriverType() != EDT_DIRECT3D11)
	{
		os::Printer::log("Fatal Error: Tried to set a texture not owned by this driver.", ELL_ERROR);
		return false;
	}

	// check for valid render target
	if (texture && !texture->isRenderTarget())
	{
		os::Printer::log("Fatal Error: Tried to set a non render target texture as render target.", ELL_ERROR);
		return false;
	}

	CD3D11Texture* tex = static_cast<CD3D11Texture*>(texture);

	// check if we should set the previous RT back
	if (tex == 0)
	{
		CurrentBackBuffer = DefaultBackBuffer;
		CurrentDepthBuffer = DefaultDepthBuffer;
		ImmediateContext->OMSetRenderTargets( 1, &CurrentBackBuffer, CurrentDepthBuffer );
		CurrentRendertargetSize = core::dimension2d<u32>(0,0);
	}
	else
	{
		CurrentBackBuffer = tex->getRenderTargetView();
		CurrentDepthBuffer = tex->DepthSurface->Surface;
		ImmediateContext->OMSetRenderTargets( 1, &CurrentBackBuffer, CurrentDepthBuffer);
		CurrentRendertargetSize = tex->getSize();
	}

	// clear views
	if (clearBackBuffer)
	{
		SColorf fCol( color );
		// swizzle clear color is texture is passed
		// i.e.: is is default render target, don't swizzle
		FLOAT c[4] = { tex == 0 ? fCol.r : fCol.b, 
					   fCol.g, 
					   tex == 0 ? fCol.b :fCol.r,
					   fCol.a };
		ImmediateContext->ClearRenderTargetView( CurrentBackBuffer, c );		
	}

	// clear depth
	if(clearZBuffer )
		this->clearZBuffer();

	// set blend
	BlendDesc.IndependentBlendEnable = FALSE;

	// don't forget to set viewport
	setViewPort(core::rect<s32>(0, 0, getCurrentRenderTargetSize().Width, getCurrentRenderTargetSize().Height));

	return true;
}

bool CD3D11Driver::setRenderTarget(const core::array<video::IRenderTarget>& targets,
			bool clearBackBuffer, bool clearZBuffer, SColor color)
{
	u32 i;
	// If no targets, set default render target
	if (!targets.size())
	{		
		// set default render target
		return setRenderTarget(0, clearBackBuffer, clearZBuffer, color);
	}

	// max number of render targets is 8 for DX11 feature level
	u32 maxMultipleRTTs = core::min_((u32)D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.size());
		
	// validation
	for (i = 0; i < maxMultipleRTTs; ++i)
	{
		if (targets[i].TargetType != ERT_RENDER_TEXTURE || !targets[i].RenderTexture)
		{
			maxMultipleRTTs = i;
			os::Printer::log("Missing texture for MRT.", ELL_WARNING);
			break;
		}

		// check for right driver type
		if (targets[i].RenderTexture->getDriverType() != EDT_DIRECT3D11)
		{
			maxMultipleRTTs = i;
			os::Printer::log("Tried to set a texture not owned by this driver.", ELL_WARNING);
			break;
		}

		// check for valid render target
		if (!targets[i].RenderTexture->isRenderTarget())
		{
			maxMultipleRTTs = i;
			os::Printer::log("Tried to set a non render target texture as render target.", ELL_WARNING);
			break;
		}

		// check for valid size
		if (targets[0].RenderTexture->getSize() != targets[i].RenderTexture->getSize())
		{
			maxMultipleRTTs = i;
			os::Printer::log("Render target texture has wrong size.", ELL_WARNING);
			break;
		}
	}
	if (!maxMultipleRTTs)
	{
		os::Printer::log("Fatal Error: No valid MRT found.", ELL_ERROR);
		return false;
	}

	CD3D11Texture* tex = static_cast<CD3D11Texture*>(targets[0].RenderTexture);

	ID3D11RenderTargetView* RTViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];

	// parse clear color
	SColorf fCol( color );
	// swizzle clear color is texture is passed
	// i.e.: is is default render target, don't swizzle
	FLOAT c[4] = { tex == 0 ? fCol.r : fCol.b, 
					fCol.g, 
					tex == 0 ? fCol.b :fCol.r,
					fCol.a };

	// zero blend description
	::ZeroMemory( &BlendDesc, sizeof( BlendDesc ) );
	BlendDesc.IndependentBlendEnable = TRUE;

	// set blend based on render target configuration
	// set source blend
	for(i = 0; i < maxMultipleRTTs; i++)
	{
		E_BLEND_FACTOR blendFac = targets[i].BlendFuncSrc;
		
		BlendDesc.RenderTarget[i].SrcBlend = BlendDesc.RenderTarget[i].SrcBlendAlpha =
			blendFac == EBF_ZERO ? D3D11_BLEND_ZERO :
			blendFac == EBF_ONE  ? D3D11_BLEND_ONE :
			blendFac == EBF_DST_COLOR ? D3D11_BLEND_DEST_COLOR :
			blendFac == EBF_ONE_MINUS_DST_COLOR ? D3D11_BLEND_INV_DEST_COLOR :
			blendFac == EBF_SRC_COLOR ? D3D11_BLEND_SRC_COLOR :
			blendFac == EBF_ONE_MINUS_SRC_COLOR ? D3D11_BLEND_INV_SRC_COLOR:
			blendFac == EBF_SRC_ALPHA ? D3D11_BLEND_SRC_ALPHA :
			blendFac == EBF_ONE_MINUS_SRC_ALPHA ? D3D11_BLEND_INV_SRC_ALPHA :
			blendFac == EBF_DST_ALPHA ? D3D11_BLEND_DEST_ALPHA :
			blendFac == EBF_ONE_MINUS_DST_ALPHA ? D3D11_BLEND_INV_DEST_ALPHA :
												  D3D11_BLEND_SRC_ALPHA_SAT ;

		// set destination blend
		blendFac = targets[i].BlendFuncDst;
		BlendDesc.RenderTarget[i].DestBlend = BlendDesc.RenderTarget[i].DestBlendAlpha =
			blendFac == EBF_ZERO ? D3D11_BLEND_ZERO :
			blendFac == EBF_ONE  ? D3D11_BLEND_ONE :
			blendFac == EBF_DST_COLOR ? D3D11_BLEND_DEST_COLOR :
			blendFac == EBF_ONE_MINUS_DST_COLOR ? D3D11_BLEND_INV_DEST_COLOR :
			blendFac == EBF_SRC_COLOR ? D3D11_BLEND_SRC_COLOR :
			blendFac == EBF_ONE_MINUS_SRC_COLOR ? D3D11_BLEND_INV_SRC_COLOR:
			blendFac == EBF_SRC_ALPHA ? D3D11_BLEND_SRC_ALPHA :
			blendFac == EBF_ONE_MINUS_SRC_ALPHA ? D3D11_BLEND_INV_SRC_ALPHA :
			blendFac == EBF_DST_ALPHA ? D3D11_BLEND_DEST_ALPHA :
			blendFac == EBF_ONE_MINUS_DST_ALPHA ? D3D11_BLEND_INV_DEST_ALPHA :
												  D3D11_BLEND_SRC_ALPHA_SAT ;

		// set blend operation
		BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		BlendDesc.RenderTarget[i].BlendEnable = targets[i].BlendOp;	

		// set blend based on render target information
		if (targets[i].BlendOp)
		{
			// set color mask
			BlendDesc.RenderTarget[i].RenderTargetWriteMask = getColorWriteEnable(targets[i].ColorMask);
		}

		// store render target view in array
		RTViews[i] = static_cast<CD3D11Texture*>(targets[i].RenderTexture)->getRenderTargetView();
		
		// clear render target view
		if( clearBackBuffer )
			ImmediateContext->ClearRenderTargetView( RTViews[i], c );
	}

	// set depth buffer
	CurrentDepthBuffer = tex->DepthSurface->Surface;
	if (clearZBuffer)
		this->clearZBuffer();

	// set targets
	ImmediateContext->OMSetRenderTargets( maxMultipleRTTs, RTViews, CurrentDepthBuffer );

	// don't forget to set viewport
	CurrentRendertargetSize = tex->getSize();
	setViewPort(core::rect<s32>(0, 0, getCurrentRenderTargetSize().Width, getCurrentRenderTargetSize().Height));

	return true;
}

void CD3D11Driver::setViewPort(const core::rect<s32>& area)
{
	core::rect<s32> vp = area;
	core::rect<s32> rendert(0,0, getCurrentRenderTargetSize().Width, getCurrentRenderTargetSize().Height);
	vp.clipAgainst(rendert);

	D3D11_VIEWPORT viewPort;
	viewPort.TopLeftX = (FLOAT)vp.UpperLeftCorner.X;
	viewPort.TopLeftY = (FLOAT)vp.UpperLeftCorner.Y;
	viewPort.Width = (FLOAT)vp.getWidth();
	viewPort.Height = (FLOAT)vp.getHeight();
	viewPort.MinDepth = 0.0f;
	viewPort.MaxDepth = 1.0f;

	ImmediateContext->RSSetViewports( 1, &viewPort );

	this->ViewPort = vp;
}

const core::rect<s32>& CD3D11Driver::getViewPort() const
{
	return ViewPort;
}

bool CD3D11Driver::updateVertexHardwareBuffer(SHWBufferLink_d3d11 *hwBuffer)
{
	if (!hwBuffer)
		return false;

	const scene::IMeshBuffer* mb = hwBuffer->MeshBuffer;

	const void* vertices = mb->getVertexBuffer()->getVertices();
	u32 vertexCount = mb->getVertexBuffer()->getVertexCount();

	E_VERTEX_TYPE vType = EVT_STANDARD;

	switch(mb->getVertexBuffer()->getVertexSize())
	{
	case sizeof(S3DVertex):
		vType = EVT_STANDARD;
		break;
	case sizeof(S3DVertex2TCoords):
		vType = EVT_2TCOORDS;
		break;
	case sizeof(S3DVertexTangents):
		vType = EVT_TANGENTS;
		break;
	default:
		return false;
	}

	const u32 vertexSize = getVertexPitchFromType(vType);

	// if vertex buffer doesn't exists OR 
	// number of vertex to update is bigger than current
	if (!hwBuffer->vertexBuffer || vertexSize * vertexCount > hwBuffer->vertexBufferSize)
	{
		// Release buffer if need to expand
		if(vertexSize * vertexCount > hwBuffer->vertexBufferSize)
		{
			SAFE_RELEASE( hwBuffer->vertexBuffer );
		}

		// Create new buffer
		D3D11_BUFFER_DESC desc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT;
		desc.ByteWidth = vertexCount * vertexSize;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.StructureByteStride = 0;

		// Specify initial data
		D3D11_SUBRESOURCE_DATA* data = 0;
		if (vertices)
		{
			data = new D3D11_SUBRESOURCE_DATA();
			data->pSysMem = vertices;
		}

		Device->CreateBuffer( &desc, data, &( hwBuffer->vertexBuffer ) );
		if(data)
			delete data;
		
		if(!hwBuffer->vertexBuffer)
			return false;
		
		hwBuffer->vertexBufferSize = vertexCount * vertexSize;
	}
	else // just update
	{
		D3D11_BOX box;
		box.left = 0;
		box.top = 0;
		box.front = 0;
		box.right = hwBuffer->vertexBufferSize;
		box.bottom = 1;
		box.back = 1;
		ImmediateContext->UpdateSubresource( hwBuffer->vertexBuffer, 0, &box, vertices, 0, 0 );
	}

	return true;
}

bool CD3D11Driver::updateIndexHardwareBuffer(SHWBufferLink_d3d11* hwBuffer)
{
	if (!hwBuffer)
		return false;

	const scene::IMeshBuffer* mb = hwBuffer->MeshBuffer;

	void* indices = mb->getIndexBuffer()->getIndices();
	u32 indexCount = mb->getIndexBuffer()->getIndexCount();
	const u32 indexSize = mb->getIndexBuffer()->getType() == EIT_16BIT ? 2 : 4;

	if (!hwBuffer->indexBuffer || indexSize * indexCount > hwBuffer->indexBufferSize)
	{
		// Release buffer if need to expand
		if(indexSize * indexCount > hwBuffer->indexBufferSize)
		{
			SAFE_RELEASE( hwBuffer->indexBuffer );
		}

		D3D11_BUFFER_DESC desc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		desc.ByteWidth = indexCount * indexSize;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.StructureByteStride = 0;
		
		// Specify initial data
		D3D11_SUBRESOURCE_DATA* data = 0;
		if (indices)
		{
			data = new D3D11_SUBRESOURCE_DATA();
			data->pSysMem = indices;
		}

		Device->CreateBuffer( &desc, data, &( hwBuffer->indexBuffer ) );
		if(data)
			delete data;
		
		if(!hwBuffer->indexBuffer)
			return false;
		
		hwBuffer->indexBufferSize = indexCount * indexSize;
	}
	else // just update 
	{
		D3D11_BOX box;
		box.left = 0;
		box.top = 0;
		box.front = 0;
		box.right = hwBuffer->indexBufferSize;
		box.bottom = 1;
		box.back = 1;
		ImmediateContext->UpdateSubresource( hwBuffer->indexBuffer, 0, &box, indices, 0, 0 );
	}

	return true;
}

bool CD3D11Driver::updateHardwareBuffer(SHWBufferLink *hwBuffer)
{
	if (!hwBuffer)
		return false;

	SHWBufferLink_d3d11* dx11Link = static_cast<SHWBufferLink_d3d11*>(hwBuffer);

	// update vertex buffer
	if (dx11Link->ChangedID_Vertex != dx11Link->MeshBuffer->getChangedID_Vertex() || 
		!dx11Link->vertexBuffer)
	{
		dx11Link->ChangedID_Vertex = dx11Link->MeshBuffer->getChangedID_Vertex();

		if (!updateVertexHardwareBuffer(dx11Link))
			return false;
	}

	// update index buffer
	if (dx11Link->ChangedID_Index != dx11Link->MeshBuffer->getChangedID_Index()	||
		!dx11Link->indexBuffer)
	{
		dx11Link->ChangedID_Index = dx11Link->MeshBuffer->getChangedID_Index();

		if (!updateIndexHardwareBuffer(dx11Link))
			return false;
	}

	return true;
}

CD3D11Driver::SHWBufferLink* CD3D11Driver::createHardwareBuffer(const scene::IMeshBuffer* mb)
{
	// DirectX 11 ALWAYS uses hardware buffer
	SHWBufferLink_d3d11* hwBuffer = new SHWBufferLink_d3d11(mb);

	//add to map
	HWBufferMap.insert(hwBuffer->MeshBuffer, hwBuffer);

	hwBuffer->ChangedID_Vertex=hwBuffer->MeshBuffer->getChangedID_Vertex();
	hwBuffer->ChangedID_Index=hwBuffer->MeshBuffer->getChangedID_Index();
	hwBuffer->Mapped_Vertex=mb->getHardwareMappingHint_Vertex();
	hwBuffer->Mapped_Index=mb->getHardwareMappingHint_Index();
	hwBuffer->LastUsed=0;

	if (!updateHardwareBuffer(hwBuffer))
	{
		deleteHardwareBuffer(hwBuffer);
		return 0;
	}

	return hwBuffer;
}

void CD3D11Driver::deleteHardwareBuffer(SHWBufferLink *_HWBuffer)
{
	if (!_HWBuffer) return;

	SHWBufferLink_d3d11* HWBuffer = static_cast<SHWBufferLink_d3d11*>(_HWBuffer);
	
	SAFE_RELEASE(HWBuffer->indexBuffer);
	SAFE_RELEASE(HWBuffer->vertexBuffer);
	SAFE_RELEASE(HWBuffer->vertexBuffer2);
	
	CNullDriver::deleteHardwareBuffer(_HWBuffer);
}

void CD3D11Driver::drawHardwareBuffer(SHWBufferLink* _HWBuffer)
{
	if (!_HWBuffer)
		return;

	SHWBufferLink_d3d11* HWBuffer = static_cast<SHWBufferLink_d3d11*>(_HWBuffer);

	updateHardwareBuffer(HWBuffer);		//check if update is needed

	HWBuffer->LastUsed=0;				//reset count

	const scene::IMeshBuffer* mb = HWBuffer->MeshBuffer;

	E_VERTEX_TYPE vType = EVT_STANDARD;

	switch(mb->getVertexBuffer()->getVertexSize())
	{
	case sizeof(S3DVertex):
		vType = EVT_STANDARD;
		break;
	case sizeof(S3DVertex2TCoords):
		vType = EVT_2TCOORDS;
		break;
	case sizeof(S3DVertexTangents):
		vType = EVT_TANGENTS;
		break;
	default:
		return;
	}

	const u32 stride = getVertexPitchFromType(vType);
	UINT offset = 0;

	// set vertex buffer
	ID3D11Buffer* buffers[1] = { HWBuffer->vertexBuffer };

	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );

	// set index buffer
	ImmediateContext->IASetIndexBuffer( HWBuffer->indexBuffer, mb->getIndexBuffer()->getType() == video::EIT_16BIT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0 );

	// draw
	drawVertexPrimitiveList(0, mb->getVertexBuffer()->getVertexCount(), 0, mb->getIndexBuffer()->getIndexCount()/3, vType, scene::EPT_TRIANGLES, mb->getIndexBuffer()->getType());

	// unset buffers
	buffers[0] = NULL;
	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );
	ImmediateContext->IASetIndexBuffer( NULL, DXGI_FORMAT_R32_UINT, 0 );
}

void CD3D11Driver::drawHardwareBuffer(IHardwareBuffer* vertices,
				IHardwareBuffer* indices, E_VERTEX_TYPE vType, 
				scene::E_PRIMITIVE_TYPE pType, E_INDEX_TYPE iType, u32 numInstances)
{
	//CNullDriver::drawHardwareBuffer(vertices, indices, vType, pType, iType);

	// Get vertex declaration
	DeclarationNode declNode = declarationMap.find(vType);
	if (!declNode)
	{
		os::Printer::log("Error, vertex type not registered", ELL_ERROR);
		return;
	}

	LastVertexType = vType;

	// Set render states
	if (!setRenderStates3DMode())
		return;

	CD3D11HardwareBuffer* vertexBuffer = static_cast<CD3D11HardwareBuffer*>(vertices);
	const u32 stride = declNode->getValue()->getVertexPitch();
	const u32 vertexCount = vertexBuffer->size() / stride;
	UINT offset = 0;

	// set vertex buffer
	ID3D11Buffer* buffers[1] = { vertexBuffer->getBufferResource() };
	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );

	// Bind textures and samplers
	ID3D11SamplerState* samplers[MATERIAL_MAX_TEXTURES];
	ID3D11ShaderResourceView* views[MATERIAL_MAX_TEXTURES];
	for(int i = 0; i < MATERIAL_MAX_TEXTURES; i++)
	{
		if( CurrentTexture[i] )
		{
			views[i] = ((CD3D11Texture*)CurrentTexture[i])->getShaderResourceView();
			samplers[i] = getSamplerState(i);
		}
		else
		{
			views[i] = 0;
			samplers[i] = 0;
		}
	}
	ImmediateContext->PSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );
	ImmediateContext->PSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->GSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );		// Test for recirculate
	ImmediateContext->GSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->VSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );	// Test for recirculate
	ImmediateContext->VSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );

	// Bind depth-stencil view
	ImmediateContext->OMSetDepthStencilState( getDepthStencilState(), 1 );

	// Bind blend state
	ImmediateContext->OMSetBlendState( getBlendState(), 0, 0xffffffff );

	// Bind rasterizer state
	ImmediateContext->RSSetState( getRasterizerState() );

	// Bind input layout
	IMaterialRenderer* renderer = MaterialRenderers[Material.MaterialType].Renderer;
	ImmediateContext->IASetInputLayout( declNode->getValue()->getInputLayout(renderer) );

	// Set topology
	ImmediateContext->IASetPrimitiveTopology( getTopology(pType) );
	
	// finally, draw
	if (indices)
	{
		// Using indices
		CD3D11HardwareBuffer* indexBuffer = static_cast<CD3D11HardwareBuffer*>(indices);
		const DXGI_FORMAT idxType = getIndexType(iType);
		const u32 idxSize = getIndexSize(iType);
		const u32 indexCount = indexBuffer->size() / idxSize;
		
		ImmediateContext->IASetIndexBuffer( indexBuffer->getBufferResource(), idxType, 0 );

		if (numInstances > 0)
			ImmediateContext->DrawIndexedInstanced( indexCount, numInstances, 0, 0, 0 );
		else
			ImmediateContext->DrawIndexed( indexCount, 0, 0 );
	}
	else
	{
		if (numInstances > 0)
			ImmediateContext->DrawInstanced( vertexCount, numInstances, 0, 0 );
		else
			ImmediateContext->Draw( vertexCount, 0 );
	}

	// unset buffers
	buffers[0] = 0;
	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );
	ImmediateContext->IASetIndexBuffer( 0, DXGI_FORMAT_R32_UINT, 0 );
	ImmediateContext->SOSetTargets( 1, buffers, &offset );
}

void CD3D11Driver::drawAuto(IHardwareBuffer* vertices, E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType)
{
	//CNullDriver::drawAuto(vertices, vType, pType);

	// Get vertex declaration
	DeclarationNode declNode = declarationMap.find(vType);
	if (!declNode)
	{
		os::Printer::log("Error, vertex type not registered", ELL_ERROR);
		return;
	}

	LastVertexType = vType;

	// Set render states
	if (!setRenderStates3DMode())
		return;

	CD3D11HardwareBuffer* vertexBuffer = static_cast<CD3D11HardwareBuffer*>(vertices);
	const u32 stride = declNode->getValue()->getVertexPitch();
	const u32 offset = 0;

	// set vertex buffer
	ID3D11Buffer* buffers[1] = { vertexBuffer->getBufferResource() };
	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );

	// Bind textures and samplers
	ID3D11SamplerState* samplers[MATERIAL_MAX_TEXTURES];
	ID3D11ShaderResourceView* views[MATERIAL_MAX_TEXTURES];
	for(int i = 0; i < MATERIAL_MAX_TEXTURES; i++)
	{
		if( CurrentTexture[i] )
		{
			views[i] = ((CD3D11Texture*)CurrentTexture[i])->getShaderResourceView();
			samplers[i] = getSamplerState(i);
		}
		else
		{
			views[i] = 0;
			samplers[i] = 0;
		}
	}
	ImmediateContext->PSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );
	ImmediateContext->PSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );	
	ImmediateContext->GSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );		// Test for recirculate
	ImmediateContext->GSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->VSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );	// Test for recirculate
	ImmediateContext->VSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );

	// Bind input layout
	IMaterialRenderer* renderer = MaterialRenderers[Material.MaterialType].Renderer;
	ImmediateContext->IASetInputLayout( declNode->getValue()->getInputLayout(renderer) );

	// Bind depth-stencil view
	ImmediateContext->OMSetDepthStencilState( getDepthStencilState(), 1 );

	// Bind blend state
	ImmediateContext->OMSetBlendState( getBlendState(), 0, 0xffffffff );

	// Bind rasterizer state
	ImmediateContext->RSSetState( getRasterizerState() );

	// Bind topology
	ImmediateContext->IASetPrimitiveTopology( getTopology(pType) );

	// Finally, draw
	ImmediateContext->DrawAuto();

	// Unset buffers, including stream output
	buffers[0] = 0;
	ImmediateContext->IASetVertexBuffers( 0, 1, buffers, &stride, &offset );
	ImmediateContext->IASetIndexBuffer( 0, DXGI_FORMAT_R32_UINT, 0 );
	ImmediateContext->SOSetTargets( 1, buffers, &offset );
}

void CD3D11Driver::drawVertexPrimitiveList(bool hardwareVertex, scene::IVertexBuffer* vertexBuffer,
	bool hardwareIndex, scene::IIndexBuffer* indexBuffer, u32 primitiveCount, scene::E_PRIMITIVE_TYPE pType)

{
	E_VERTEX_TYPE vType = EVT_STANDARD;

	// Supported are only built-in Irrlicht vertex formats.
	switch(vertexBuffer->getVertexSize())
	{
	case sizeof(S3DVertex):
		vType = EVT_STANDARD;
		break;
	case sizeof(S3DVertex2TCoords):
		vType = EVT_2TCOORDS;
		break;
	case sizeof(S3DVertexTangents):
		vType = EVT_TANGENTS;
		break;
	default:
		return;
	}

	drawVertexPrimitiveList(vertexBuffer->getVertices(), vertexBuffer->getVertexCount(), indexBuffer->getIndices(), primitiveCount, vType, pType, indexBuffer->getType());
}

void CD3D11Driver::drawVertexPrimitiveList(const void* vertices, u32 vertexCount,
										   const void* indexList, u32 primitiveCount,
										   E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType,
										   E_INDEX_TYPE iType)
{
	if (!checkPrimitiveCount(primitiveCount))
		return;

	// Emulate CNullDriver::drawVertexPrimitiveList call.
	if((iType == EIT_16BIT) && (vertexCount > 65536))
		os::Printer::log("Too many vertices for 16bit index type, render artifacts may occur.");

	PrimitivesDrawn += primitiveCount;

	if (!vertexCount || !primitiveCount)
		return;

	draw2D3DVertexPrimitiveList(vertices, vertexCount, indexList, primitiveCount,
		vType, pType, iType, true);
 }

void CD3D11Driver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount,
											 const void* indexList, u32 primitiveCount,
											 E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType,
											 E_INDEX_TYPE iType)
{
	if (!checkPrimitiveCount(primitiveCount))
		return;

	CNullDriver::draw2DVertexPrimitiveList(vertices, vertexCount, indexList, primitiveCount, vType, pType, iType);

	if (!vertexCount || !primitiveCount)
		return;	
	
	// Draw
	// Bind textures and samplers
	ID3D11SamplerState* samplers[MATERIAL_MAX_TEXTURES];
	ID3D11ShaderResourceView* views[MATERIAL_MAX_TEXTURES];

	for(int i = 0; i < MATERIAL_MAX_TEXTURES; i++)
	{
		if( CurrentTexture[i] )
		{
			views[i] = ((CD3D11Texture*)CurrentTexture[i])->getShaderResourceView();
			samplers[i] = getSamplerState(i);
		}
		else
		{
			views[i] = 0;
			samplers[i] = 0;
		}
	}

	ImmediateContext->PSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );
	ImmediateContext->PSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->GSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );		// Test for recirculate
	ImmediateContext->GSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->VSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );	// Test for recirculate
	ImmediateContext->VSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );

	// Bind depth-stencil view
	ImmediateContext->OMSetDepthStencilState( getDepthStencilState(), 1 );

	// Bind blend state
	ImmediateContext->OMSetBlendState( getBlendState(), 0, 0xffffffff );

	// Bind rasterizer state
	ImmediateContext->RSSetState( getRasterizerState() );

	// Bind input layout
	DeclarationNode declNode = declarationMap.find(vType);
	if (declNode)
	{
		IMaterialRenderer* renderer = MaterialRenderers[Material.MaterialType].Renderer;
		ImmediateContext->IASetInputLayout( declNode->getValue()->getInputLayout(renderer) );
	}

	// copy vertices to dynamic buffers, if needed
	if(vertices || indexList)	
		uploadVertexData(vertices, vertexCount, indexList, getIndexAmount(pType, primitiveCount), vType, iType);

	// finally, draw
	ImmediateContext->IASetPrimitiveTopology(getTopology(pType));

	if (pType == scene::EPT_POINTS || pType == scene::EPT_POINT_SPRITES)
		ImmediateContext->Draw( vertexCount, 0 );
	else if (vertexCount == 0)
		ImmediateContext->DrawAuto();
	else
		ImmediateContext->DrawIndexed(getIndexAmount(pType, primitiveCount), 0, 0 );
}

void CD3D11Driver::draw2D3DVertexPrimitiveList(const void* vertices,
		u32 vertexCount, const void* indexList, u32 primitiveCount,
		E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType,
		E_INDEX_TYPE iType, bool is3D)
{
	LastVertexType = vType;

	if (is3D)
	{
		if(!setRenderStates3DMode())
			return;
	}
	else
	{
		if (Material.MaterialType==EMT_ONETEXTURE_BLEND)
		{
			E_BLEND_FACTOR srcFact;
			E_BLEND_FACTOR dstFact;
			E_MODULATE_FUNC modulo;
			u32 alphaSource;
			unpack_texureBlendFunc ( srcFact, dstFact, modulo, alphaSource, Material.MaterialTypeParam);
			setRenderStates2DMode(alphaSource & video::EAS_VERTEX_COLOR, (Material.getTexture(0) != 0), (alphaSource&video::EAS_TEXTURE) != 0);
		}
		else
			setRenderStates2DMode(Material.MaterialType==EMT_TRANSPARENT_VERTEX_ALPHA, (Material.getTexture(0) != 0), Material.MaterialType==EMT_TRANSPARENT_ALPHA_CHANNEL);
	}

	// Bind textures and samplers
	ID3D11SamplerState* samplers[MATERIAL_MAX_TEXTURES];
	ID3D11ShaderResourceView* views[MATERIAL_MAX_TEXTURES];

	for(int i = 0; i < MATERIAL_MAX_TEXTURES; i++)
	{
		if( CurrentTexture[i] )
		{
			views[i] = ((CD3D11Texture*)CurrentTexture[i])->getShaderResourceView();
			samplers[i] = getSamplerState(i);
		}
		else
		{
 			views[i] = 0;
			samplers[i] = 0;
		}
	}

	ImmediateContext->PSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );
	ImmediateContext->PSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->GSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );		// Test for recirculate
	ImmediateContext->GSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );
	ImmediateContext->VSSetShaderResources( 0, MATERIAL_MAX_TEXTURES, views );	// Test for recirculate
	ImmediateContext->VSSetSamplers( 0, MATERIAL_MAX_TEXTURES, samplers );

	// Bind depth-stencil view
	ImmediateContext->OMSetDepthStencilState( getDepthStencilState(), 1 );

	// Bind blend state
	ImmediateContext->OMSetBlendState( getBlendState(), 0, 0xffffffff );

	// Bind rasterizer state
	ImmediateContext->RSSetState( getRasterizerState() );

	// Bind input layout
	DeclarationNode declNode = declarationMap.find(vType);
	if (declNode)
	{
		IMaterialRenderer* renderer = MaterialRenderers[Material.MaterialType].Renderer;
		ImmediateContext->IASetInputLayout( declNode->getValue()->getInputLayout(renderer) );
	}

	// copy vertices to dynamic buffers, if needed
	if(vertices || indexList)	
		uploadVertexData(vertices, vertexCount, indexList, getIndexAmount(pType, primitiveCount), vType, iType);

	// finally, draw
	ImmediateContext->IASetPrimitiveTopology( getTopology(pType) );
	if (pType == scene::EPT_POINTS || pType == scene::EPT_POINT_SPRITES)
		ImmediateContext->Draw( vertexCount, 0 );
	else if (vertexCount == 0)
		ImmediateContext->DrawAuto();
	else
		ImmediateContext->DrawIndexed( getIndexAmount(pType, primitiveCount), 0, 0 );
}

void CD3D11Driver::draw2DImage(const video::ITexture* texture, 
							   const core::rect<s32>& destRect,
							   const core::rect<s32>& sourceRect, 
							   const core::rect<s32>* clipRect,
							   const video::SColor* const colors, 
							   bool useAlphaChannelOfTexture)
{
	if(!texture || texture->getDriverType() != EDT_DIRECT3D11)
		return;

	if(!setActiveTexture(0, const_cast<video::ITexture*>(texture)))
		return;

	setActiveTexture(1, 0);

	const video::SColor temp[4] =
	{
		0xFFFFFFFF,
		0xFFFFFFFF,
		0xFFFFFFFF,
		0xFFFFFFFF
	};

	const video::SColor* const useColor = colors ? colors : temp;

	const core::dimension2d<u32>& ss = texture->getOriginalSize();
	const core::dimension2d<u32>& renderTargetSize = getCurrentRenderTargetSize();

	// clip source and destination rects to prevent draw pixels outside draw area.
	core::rect<f32> tcoords;
	tcoords.UpperLeftCorner.X = (f32)sourceRect.UpperLeftCorner.X / (f32)ss.Width;
	tcoords.UpperLeftCorner.Y = (f32)sourceRect.UpperLeftCorner.Y / (f32)ss.Height;
	tcoords.LowerRightCorner.X = (f32)sourceRect.LowerRightCorner.X / (f32)ss.Width;
	tcoords.LowerRightCorner.Y = (f32)sourceRect.LowerRightCorner.Y / (f32)ss.Height;

	S3DVertex vtx[4]; // clock wise
	vtx[0] = S3DVertex((f32)destRect.UpperLeftCorner.X, (f32)destRect.UpperLeftCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, useColor[0],
		tcoords.UpperLeftCorner.X, tcoords.UpperLeftCorner.Y);
	vtx[1] = S3DVertex((f32)destRect.LowerRightCorner.X, (f32)destRect.UpperLeftCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, useColor[3],
		tcoords.LowerRightCorner.X, tcoords.UpperLeftCorner.Y);
	vtx[2] = S3DVertex((f32)destRect.LowerRightCorner.X, (f32)destRect.LowerRightCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, useColor[2],
		tcoords.LowerRightCorner.X, tcoords.LowerRightCorner.Y);
	vtx[3] = S3DVertex((f32)destRect.UpperLeftCorner.X, (f32)destRect.LowerRightCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, useColor[1],
		tcoords.UpperLeftCorner.X, tcoords.LowerRightCorner.Y);

	s32 verticesSize = sizeof(vtx)/sizeof(vtx[0]);

	s16 indices[6] = {0,1,2,0,2,3};
	s32 indicesSize = sizeof(indices)/sizeof(indices[0]);
	
	LastVertexType = EVT_STANDARD;

	setRenderStates2DMode(useColor[0].getAlpha()<255 || 
						  useColor[1].getAlpha()<255 ||
						  useColor[2].getAlpha()<255 || 
		                  useColor[3].getAlpha()<255,
						  true, useAlphaChannelOfTexture);

	// Draw
	draw2DVertexPrimitiveList(vtx, verticesSize, indices, indicesSize/3, video::EVT_STANDARD, scene::EPT_TRIANGLES, video::EIT_16BIT);
}

void CD3D11Driver::draw2DImage(const video::ITexture* texture, 
							   const core::position2d<s32>& pos,
							   const core::rect<s32>& sourceRect, 
							   const core::rect<s32>* clipRect,
							   SColor color, 
							   bool useAlphaChannelOfTexture)
{
	if (!texture)
		return;

	if (!sourceRect.isValid())
		return;

	if (!setActiveTexture(0, const_cast<video::ITexture*>(texture)))
		return;

	setActiveTexture(1, 0);

	core::position2d<s32> targetPos = pos;
	core::position2d<s32> sourcePos = sourceRect.UpperLeftCorner;
	// This needs to be signed as it may go negative.
	core::dimension2d<s32> sourceSize(sourceRect.getSize());

	if (clipRect)
	{
		if (targetPos.X < clipRect->UpperLeftCorner.X)
		{
			sourceSize.Width += targetPos.X - clipRect->UpperLeftCorner.X;
			if (sourceSize.Width <= 0)
				return;

			sourcePos.X -= targetPos.X - clipRect->UpperLeftCorner.X;
			targetPos.X = clipRect->UpperLeftCorner.X;
		}

		if (targetPos.X + (s32)sourceSize.Width > clipRect->LowerRightCorner.X)
		{
			sourceSize.Width -= (targetPos.X + sourceSize.Width) - clipRect->LowerRightCorner.X;
			if (sourceSize.Width <= 0)
				return;
		}

		if (targetPos.Y < clipRect->UpperLeftCorner.Y)
		{
			sourceSize.Height += targetPos.Y - clipRect->UpperLeftCorner.Y;
			if (sourceSize.Height <= 0)
				return;

			sourcePos.Y -= targetPos.Y - clipRect->UpperLeftCorner.Y;
			targetPos.Y = clipRect->UpperLeftCorner.Y;
		}

		if (targetPos.Y + (s32)sourceSize.Height > clipRect->LowerRightCorner.Y)
		{
			sourceSize.Height -= (targetPos.Y + sourceSize.Height) - clipRect->LowerRightCorner.Y;
			if (sourceSize.Height <= 0)
				return;
		}
	}

	// clip these coordinates

	if (targetPos.X<0)
	{
		sourceSize.Width += targetPos.X;
		if (sourceSize.Width <= 0)
			return;

		sourcePos.X -= targetPos.X;
		targetPos.X = 0;
	}

	const core::dimension2d<u32>& renderTargetSize = getCurrentRenderTargetSize();

	if (targetPos.X + sourceSize.Width > (s32)renderTargetSize.Width)
	{
		sourceSize.Width -= (targetPos.X + sourceSize.Width) - renderTargetSize.Width;
		if (sourceSize.Width <= 0)
			return;
	}

	if (targetPos.Y<0)
	{
		sourceSize.Height += targetPos.Y;
		if (sourceSize.Height <= 0)
			return;

		sourcePos.Y -= targetPos.Y;
		targetPos.Y = 0;
	}

	if (targetPos.Y + sourceSize.Height > (s32)renderTargetSize.Height)
	{
		sourceSize.Height -= (targetPos.Y + sourceSize.Height) - renderTargetSize.Height;
		if (sourceSize.Height <= 0)
			return;
	}

	// ok, we've clipped everything.
	// now draw it.
	// 
	core::rect<f32> tcoords;
	tcoords.UpperLeftCorner.X = (((f32)sourcePos.X)) / texture->getOriginalSize().Width ;
	tcoords.UpperLeftCorner.Y = (((f32)sourcePos.Y)) / texture->getOriginalSize().Height;
	tcoords.LowerRightCorner.X = tcoords.UpperLeftCorner.X + ((f32)(sourceSize.Width) / texture->getOriginalSize().Width);
	tcoords.LowerRightCorner.Y = tcoords.UpperLeftCorner.Y + ((f32)(sourceSize.Height) / texture->getOriginalSize().Height);

	const core::rect<s32> poss(targetPos, sourceSize);

	S3DVertex vtx[4];
	vtx[0] = S3DVertex((f32)poss.UpperLeftCorner.X, (f32)poss.UpperLeftCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, color,
		tcoords.UpperLeftCorner.X, tcoords.UpperLeftCorner.Y);
	vtx[1] = S3DVertex((f32)poss.LowerRightCorner.X, (f32)poss.UpperLeftCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, color,
		tcoords.LowerRightCorner.X, tcoords.UpperLeftCorner.Y);
	vtx[2] = S3DVertex((f32)poss.LowerRightCorner.X, (f32)poss.LowerRightCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, color,
		tcoords.LowerRightCorner.X, tcoords.LowerRightCorner.Y);
	vtx[3] = S3DVertex((f32)poss.UpperLeftCorner.X, (f32)poss.LowerRightCorner.Y, 0.0f,
		0.0f, 0.0f, 0.0f, color,
		tcoords.UpperLeftCorner.X, tcoords.LowerRightCorner.Y);

	s32 verticesSize = sizeof(vtx)/sizeof(vtx[0]);

	s16 indices[6] = {0,1,2,0,2,3};
	s32 indicesSize = sizeof(indices)/sizeof(indices[0]);

	LastVertexType = EVT_STANDARD;

	setRenderStates2DMode(color.getAlpha()<255, true, useAlphaChannelOfTexture);

	draw2DVertexPrimitiveList(vtx, verticesSize, indices, indicesSize / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);
}

void CD3D11Driver::draw2DImageBatch(const video::ITexture* texture,
									const core::array<core::position2d<s32> >& positions,
									const core::array<core::rect<s32> >& sourceRects,
									const core::rect<s32>* clipRect,
									SColor color, bool useAlphaChannelOfTexture)
{
	if (!texture)
		return;

	if (!setActiveTexture(0, const_cast<video::ITexture*>(texture)))
		return;

	const irr::u32 drawCount = core::min_<u32>(positions.size(), sourceRects.size());
	const core::dimension2d<u32>& renderTargetSize = getCurrentRenderTargetSize();

	core::array<S3DVertex> vtx(drawCount * 4);
	core::array<u16> indices(drawCount * 6);

	for(u32 i = 0;i < drawCount;i++)
	{
		core::position2d<s32> targetPos = positions[i];
		core::position2d<s32> sourcePos = sourceRects[i].UpperLeftCorner;
		// This needs to be signed as it may go negative.
		core::dimension2d<s32> sourceSize(sourceRects[i].getSize());

		if (clipRect)
		{
			if (targetPos.X < clipRect->UpperLeftCorner.X)
			{
				sourceSize.Width += targetPos.X - clipRect->UpperLeftCorner.X;
				if (sourceSize.Width <= 0)
					continue;

				sourcePos.X -= targetPos.X - clipRect->UpperLeftCorner.X;
				targetPos.X = clipRect->UpperLeftCorner.X;
			}

			if (targetPos.X + (s32)sourceSize.Width > clipRect->LowerRightCorner.X)
			{
				sourceSize.Width -= (targetPos.X + sourceSize.Width) - clipRect->LowerRightCorner.X;
				if (sourceSize.Width <= 0)
					continue;
			}

			if (targetPos.Y < clipRect->UpperLeftCorner.Y)
			{
				sourceSize.Height += targetPos.Y - clipRect->UpperLeftCorner.Y;
				if (sourceSize.Height <= 0)
					continue;

				sourcePos.Y -= targetPos.Y - clipRect->UpperLeftCorner.Y;
				targetPos.Y = clipRect->UpperLeftCorner.Y;
			}

			if (targetPos.Y + (s32)sourceSize.Height > clipRect->LowerRightCorner.Y)
			{
				sourceSize.Height -= (targetPos.Y + sourceSize.Height) - clipRect->LowerRightCorner.Y;
				if (sourceSize.Height <= 0)
					continue;
			}
		}

		// clip these coordinates

		if (targetPos.X<0)
		{
			sourceSize.Width += targetPos.X;
			if (sourceSize.Width <= 0)
				continue;

			sourcePos.X -= targetPos.X;
			targetPos.X = 0;
		}

		if (targetPos.X + sourceSize.Width > (s32)renderTargetSize.Width)
		{
			sourceSize.Width -= (targetPos.X + sourceSize.Width) - renderTargetSize.Width;
			if (sourceSize.Width <= 0)
				continue;
		}

		if (targetPos.Y<0)
		{
			sourceSize.Height += targetPos.Y;
			if (sourceSize.Height <= 0)
				continue;

			sourcePos.Y -= targetPos.Y;
			targetPos.Y = 0;
		}

		if (targetPos.Y + sourceSize.Height > (s32)renderTargetSize.Height)
		{
			sourceSize.Height -= (targetPos.Y + sourceSize.Height) - renderTargetSize.Height;
			if (sourceSize.Height <= 0)
				continue;
		}

		// ok, we've clipped everything.
		// now draw it.

		core::rect<f32> tcoords;
		tcoords.UpperLeftCorner.X = (((f32)sourcePos.X)) / texture->getOriginalSize().Width ;
		tcoords.UpperLeftCorner.Y = (((f32)sourcePos.Y)) / texture->getOriginalSize().Height;
		tcoords.LowerRightCorner.X = tcoords.UpperLeftCorner.X + ((f32)(sourceSize.Width) / texture->getOriginalSize().Width);
		tcoords.LowerRightCorner.Y = tcoords.UpperLeftCorner.Y + ((f32)(sourceSize.Height) / texture->getOriginalSize().Height);

		const core::rect<s32> poss(targetPos, sourceSize);

		vtx.push_back(S3DVertex((f32)poss.UpperLeftCorner.X, (f32)poss.UpperLeftCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, color,
			tcoords.UpperLeftCorner.X, tcoords.UpperLeftCorner.Y));
		vtx.push_back(S3DVertex((f32)poss.LowerRightCorner.X, (f32)poss.UpperLeftCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, color,
			tcoords.LowerRightCorner.X, tcoords.UpperLeftCorner.Y));
		vtx.push_back(S3DVertex((f32)poss.LowerRightCorner.X, (f32)poss.LowerRightCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, color,
			tcoords.LowerRightCorner.X, tcoords.LowerRightCorner.Y));
		vtx.push_back(S3DVertex((f32)poss.UpperLeftCorner.X, (f32)poss.LowerRightCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, color,
			tcoords.UpperLeftCorner.X, tcoords.LowerRightCorner.Y));

		const u32 curPos = vtx.size()-4;
		indices.push_back(0+curPos);
		indices.push_back(1+curPos);
		indices.push_back(2+curPos);

		indices.push_back(0+curPos);
		indices.push_back(2+curPos);
		indices.push_back(3+curPos);
	}

	if (vtx.size())
	{	
		LastVertexType = EVT_STANDARD;

		setRenderStates2DMode(color.getAlpha()<255, true, useAlphaChannelOfTexture);

		// Draw
		draw2DVertexPrimitiveList(vtx.pointer(), vtx.size(), indices.pointer(), indices.size() / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);
	}
}

void CD3D11Driver::draw2DRectangle(const core::rect<s32>& position,
								   SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown,
								   const core::rect<s32>* clip)
{
	core::rect<s32> pos(position);

	if (clip)
		pos.clipAgainst(*clip);

	if (!pos.isValid())
		return;

	S3DVertex vtx[4];
	vtx[0] = S3DVertex((f32)pos.UpperLeftCorner.X, (f32)pos.UpperLeftCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, colorLeftUp, 0.0f, 0.0f);
	vtx[1] = S3DVertex((f32)pos.LowerRightCorner.X, (f32)pos.UpperLeftCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, colorRightUp, 0.0f, 1.0f);
	vtx[2] = S3DVertex((f32)pos.LowerRightCorner.X, (f32)pos.LowerRightCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, colorRightDown, 1.0f, 0.0f);
	vtx[3] = S3DVertex((f32)pos.UpperLeftCorner.X, (f32)pos.LowerRightCorner.Y, 0.0f,
			0.0f, 0.0f, 0.0f, colorLeftDown, 1.0f, 1.0f);

	s16 indices[6] = {0,1,2,0,2,3};

	LastVertexType = EVT_STANDARD;

	setRenderStates2DMode(
		colorLeftUp.getAlpha() < 255 ||
		colorRightUp.getAlpha() < 255 || 
		colorLeftDown.getAlpha() < 255 ||
		colorRightDown.getAlpha() < 255, false, false);

	setActiveTexture(0,0);

	// Draw
	draw2DVertexPrimitiveList(vtx, sizeof(vtx)/sizeof(vtx[0]), indices, sizeof(indices)/sizeof(indices[0]), video::EVT_STANDARD, scene::EPT_TRIANGLES, video::EIT_16BIT);
}

void CD3D11Driver::draw2DLine(const core::position2d<s32>& start,
							  const core::position2d<s32>& end, SColor color)
{
	if (start==end)
		drawPixel(start.X, start.Y, color);
	else
	{
		setActiveTexture(0,0);

		// thanks to Vash TheStampede who sent in his implementation
		S3DVertex vtx[2];
		vtx[0] = S3DVertex((f32)start.X+0.375f, (f32)start.Y+0.375f, 0.0f,
			0.0f, 0.0f, 0.0f, // normal
			color, 0.0f, 0.0f); // texture

		vtx[1] = S3DVertex((f32)end.X+0.375f, (f32)end.Y+0.375f, 0.0f,
			0.0f, 0.0f, 0.0f,
			color, 0.0f, 0.0f);

		LastVertexType = EVT_STANDARD;

		setRenderStates2DMode(color.getAlpha() < 255, false, false);

		draw2DVertexPrimitiveList(vtx, 2, 0, 1, EVT_STANDARD, scene::EPT_LINES, EIT_16BIT);
	}
}

void CD3D11Driver::drawPixel(u32 x, u32 y, const SColor & color)
{
	const core::dimension2d<u32>& renderTargetSize = getCurrentRenderTargetSize();

	if(x > (u32)renderTargetSize.Width || y > (u32)renderTargetSize.Height)
		return;

	setActiveTexture(0,0);

	S3DVertex vertex((f32)x+0.375f, (f32)y+0.375f, 0.f, 0.f, 0.f, 0.f, color, 0.f, 0.f);

	LastVertexType = EVT_STANDARD;

	setRenderStates2DMode(color.getAlpha() < 255, false, false);

	draw2DVertexPrimitiveList(&vertex, 1, 0, 1, EVT_STANDARD, scene::EPT_POINTS, EIT_16BIT);
}

void CD3D11Driver::draw3DLine(const core::vector3df& start,
							  const core::vector3df& end, SColor color)
{
	video::S3DVertex v[2];
	v[0].Color = color;
	v[1].Color = color;
	v[0].Pos = start;
	v[1].Pos = end;

	s16 indices[2] = {0,1};

	drawVertexPrimitiveList(v, 2, indices, 1, EVT_STANDARD, scene::EPT_LINES, EIT_16BIT);
}

const wchar_t* CD3D11Driver::getName() const
{
	return L"Direct3D 11.0";
}

void CD3D11Driver::deleteAllDynamicLights()
{
	RequestedLights.clear();

	CNullDriver::deleteAllDynamicLights();
}

s32 CD3D11Driver::addDynamicLight(const SLight& light)
{
	CNullDriver::addDynamicLight(light);

	RequestedLights.push_back(RequestedLight(light));

	return (s32)(RequestedLights.size() - 1);
}

void CD3D11Driver::turnLightOn(s32 lightIndex, bool turnOn)
{
	if(lightIndex < 0 || lightIndex >= (s32)RequestedLights.size())
		return;

	RequestedLight & requestedLight = RequestedLights[lightIndex];
	requestedLight.DesireToBeOn = turnOn;
}

u32 CD3D11Driver::getMaximalDynamicLightAmount() const
{
	return MaxActiveLights;
}

void CD3D11Driver::setAmbientLight(const SColorf& color)
{
	AmbientLight = color;
}

void CD3D11Driver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail, u32 debugDataVisible)
{
	if (!Params.Stencilbuffer)
		return;

	setRenderStatesStencilShadowMode(zfail, debugDataVisible);

	const u32 count = triangles.size();
	if (!count)
		return;

	if (!zfail)
	{
		// ZPASS Method

		MaterialRenderers[Material.MaterialType].Renderer->OnRender(this, EVT_STANDARD);
		// Draw front-side of shadow volume in stencil only
		RasterizerDesc.CullMode = D3D11_CULL_BACK;
		DepthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_INCR;

		draw2DVertexPrimitiveList(triangles.const_pointer(), count, 0, count / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);

		// Now reverse cull order so front sides of shadow volume are written.
		RasterizerDesc.CullMode = D3D11_CULL_FRONT;
		DepthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_DECR;
		
		draw2DVertexPrimitiveList(triangles.const_pointer(), count, 0, count / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);
	}
	else
	{
		// ZFAIL Method
		MaterialRenderers[Material.MaterialType].Renderer->OnRender(this, EVT_STANDARD);
		// Draw front-side of shadow volume in stencil only
		RasterizerDesc.CullMode = D3D11_CULL_FRONT;
		DepthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_INCR;

		draw2DVertexPrimitiveList(triangles.const_pointer(), count, 0, count / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);

		// Now reverse cull order so front sides of shadow volume are written.
		RasterizerDesc.CullMode = D3D11_CULL_BACK;
		DepthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_DECR;

		draw2DVertexPrimitiveList(triangles.const_pointer(), count, 0, count / 3, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);
	}
}

void CD3D11Driver::drawStencilShadow(bool clearStencilBuffer,
			video::SColor leftUpEdge, video::SColor rightUpEdge,
			video::SColor leftDownEdge, video::SColor rightDownEdge)
{
	if (!Params.Stencilbuffer)
		return;

	S3DVertex vtx[4];
	vtx[0] = S3DVertex(1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, leftUpEdge, 0.0f, 0.0f);
	vtx[1] = S3DVertex(1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, rightUpEdge, 0.0f, 1.0f);
	vtx[2] = S3DVertex(-1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, leftDownEdge, 1.0f, 0.0f);
	vtx[3] = S3DVertex(-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, rightDownEdge, 1.0f, 1.0f);

	s16 indices[6] = {0,1,2,1,3,2};

	setRenderStatesStencilFillMode(
		leftUpEdge.getAlpha() < 255 ||
		rightUpEdge.getAlpha() < 255 ||
		leftDownEdge.getAlpha() < 255 ||
		rightDownEdge.getAlpha() < 255);

	setActiveTexture(0,0);

	//MaterialRenderers[Material.MaterialType].Renderer->OnRender(this, EVT_STANDARD);

	LastVertexType = EVT_STANDARD;

	draw2DVertexPrimitiveList(vtx, 4, indices, 2, EVT_STANDARD, scene::EPT_TRIANGLES, EIT_16BIT);

	if (clearStencilBuffer)
		ImmediateContext->ClearDepthStencilView(DefaultDepthBuffer, D3D11_CLEAR_STENCIL, 1.0f, 0);
}

//! sets the needed renderstates
void CD3D11Driver::setRenderStatesStencilShadowMode(bool zfail, u32 debugDataVisible)
{
	if ((CurrentRenderMode != ERM_SHADOW_VOLUME_ZFAIL &&
		CurrentRenderMode != ERM_SHADOW_VOLUME_ZPASS) ||
		Transformation3DChanged)
	{
		// unset last 3d material
		if (CurrentRenderMode == ERM_3D &&
			static_cast<u32>(Material.MaterialType) < MaterialRenderers.size())
		{
			MaterialRenderers[Material.MaterialType].Renderer->OnUnsetMaterial();
			ResetRenderStates = true;
		}

		Transformation3DChanged = false;

		for(u32 i = 0; i < MATERIAL_MAX_TEXTURES; i++)
			setActiveTexture(i, 0);

		LastVertexType = (video::E_VERTEX_TYPE)(-1);
		
		DepthStencilDesc.StencilEnable = true;
		Material.GouraudShading = false;
		Material.FogEnable = false;
		Material.ZWriteEnable = false;

		DepthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DepthStencilDesc.StencilReadMask = 255;
		DepthStencilDesc.StencilWriteMask = 255;

		BlendDesc.RenderTarget[0].BlendEnable = true;
		BlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
		BlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;

		DepthStencilDesc.DepthEnable = true;
		DepthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

		if ((debugDataVisible & scene::EDS_MESH_WIRE_OVERLAY))
			RasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
	}

	if (CurrentRenderMode != ERM_SHADOW_VOLUME_ZPASS && !zfail)
	{
		// USE THE ZPASS METHOD
		DepthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		DepthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;

		//pID3DDevice->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCR);	// does not matter, will be set later
	}
	else if (CurrentRenderMode != ERM_SHADOW_VOLUME_ZFAIL && zfail)
	{
		// USE THE ZFAIL METHOD
		DepthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		//pID3DDevice->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_INCR);	// does not matter, will be set later
		DepthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP; 
	}

	CurrentRenderMode = zfail ? ERM_SHADOW_VOLUME_ZFAIL : ERM_SHADOW_VOLUME_ZPASS;
}

//! sets the needed renderstates
void CD3D11Driver::setRenderStatesStencilFillMode(bool alpha)
{
	if (CurrentRenderMode != ERM_STENCIL_FILL || Transformation3DChanged)
	{
		setTransform(ETS_VIEW, core::IdentityMatrix);
		setTransform(ETS_WORLD, core::IdentityMatrix);
		setTransform(ETS_PROJECTION, core::IdentityMatrix);

		DepthStencilDesc.DepthEnable = false;
		Material.Lighting = false;
		Material.FogEnable = false;

		DepthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_LESS_EQUAL;
		DepthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		DepthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		DepthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		DepthStencilDesc.StencilWriteMask = 255;
		DepthStencilDesc.StencilReadMask = 255;

		RasterizerDesc.CullMode = D3D11_CULL_BACK;

		Transformation3DChanged = false;

		if (alpha)
		{
			BlendDesc.RenderTarget[0].BlendEnable = true;
			BlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			BlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		}
		else
		{
			BlendDesc.RenderTarget[0].BlendEnable = false;
		}
	}

	CurrentRenderMode = ERM_STENCIL_FILL;
}

u32 CD3D11Driver::getMaximalPrimitiveCount() const
{
	return 0x7fffffff;
}


void CD3D11Driver::setTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag, bool enabled)
{
	if (flag == video::ETCF_CREATE_MIP_MAPS && !queryFeature(EVDF_MIP_MAP))
		enabled = false;

	CNullDriver::setTextureCreationFlag(flag, enabled);
}


void CD3D11Driver::OnResize(const core::dimension2d<u32>& size)
{
	if (!Device || !SwapChain)
		return;

	CNullDriver::OnResize(size);
	present.BufferDesc.Width = size.Width;
	present.BufferDesc.Height = size.Height;
	ScreenSize = size;

	reset();
}

//! sets the needed renderstates
bool CD3D11Driver::setRenderStates3DMode()
{
	if (!Device)
		return false;

	if (CurrentRenderMode != ERM_3D)
	{
		Transformation3DChanged = true;

		DepthStencilDesc.StencilEnable = false;

		ResetRenderStates = true;
	}

	if (ResetRenderStates || LastMaterial != Material)
	{
		// unset old material
		if (CurrentRenderMode == ERM_3D &&
			LastMaterial.MaterialType != Material.MaterialType &&
			LastMaterial.MaterialType >= 0 && LastMaterial.MaterialType < (s32)MaterialRenderers.size())
			MaterialRenderers[LastMaterial.MaterialType].Renderer->OnUnsetMaterial();

		// set new material.
		if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
			MaterialRenderers[Material.MaterialType].Renderer->OnSetMaterial(
			Material, LastMaterial, ResetRenderStates, this);
	}

	bool shaderOK = true;
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
		shaderOK = MaterialRenderers[Material.MaterialType].Renderer->OnRender(this, LastVertexType);

	ResetRenderStates = false;

	CurrentRenderMode = ERM_3D;

	LastMaterial = Material;

	return shaderOK;
}

void CD3D11Driver::setRenderStates2DMode(bool alpha, bool texture, bool alphaChannel)
{
	if(!Device)
		return;

	if (CurrentRenderMode != ERM_2D || Transformation3DChanged)
	{
		// unset last 3d material
		if (CurrentRenderMode == ERM_3D)
		{
			if (static_cast<u32>(LastMaterial.MaterialType) < MaterialRenderers.size())
				MaterialRenderers[LastMaterial.MaterialType].Renderer->OnUnsetMaterial();
		}

		if(!OverrideMaterial2DEnabled)
		{	
			Material = InitMaterial2D;
			MaterialRenderers[InitMaterial2D.MaterialType].Renderer->OnSetMaterial(Material, LastMaterial, true, this);
			LastMaterial = InitMaterial2D;

			DepthStencilDesc.StencilEnable = FALSE;
		}

		core::matrix4 m;

		// Set world to identity
		setTransform(ETS_WORLD, core::IdentityMatrix);

		// Set view to identity
		setTransform(ETS_VIEW, core::IdentityMatrix);

		// Adjust projection
		const core::dimension2d<u32>& renderTargetSize = getCurrentRenderTargetSize();
		m.buildProjectionMatrixOrthoLH(f32(renderTargetSize.Width), f32(-(s32)(renderTargetSize.Height)), -1.0, 1.0);
		m.setTranslation(core::vector3df(-1,1,0));
		setTransform(ETS_PROJECTION, m);

		Transformation3DChanged = false;
	}

	if (OverrideMaterial2DEnabled)
	{
		OverrideMaterial2D.Lighting = false;

		Material = OverrideMaterial2D;
		MaterialRenderers[InitMaterial2D.MaterialType].Renderer->OnSetMaterial(Material, LastMaterial, true, this);
		LastMaterial = OverrideMaterial2D;
	}

	// no alphaChannel without texture
	alphaChannel &= texture;

	if (alpha || alphaChannel)
	{
		BlendDesc.RenderTarget[0].BlendEnable = true;
		BlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		BlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	}
	else
		BlendDesc.RenderTarget[0].BlendEnable = false;

	if (texture)
	{
		setTransform(ETS_TEXTURE_0, core::IdentityMatrix);

		Transformation3DChanged = false;
	}

	MaterialRenderers[Material.MaterialType].Renderer->OnRender(this, video::EVT_STANDARD);

	CurrentRenderMode = ERM_2D;
}

void CD3D11Driver::setBasicRenderStates(const SMaterial& material, const SMaterial& lastMaterial,
										bool resetAllRenderstates)
{
	if(resetAllRenderstates)
	{
		// init states description
		BlendDesc.reset();
		RasterizerDesc.reset();
		DepthStencilDesc.reset();
		for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; i++)
			SamplerDesc[i].reset();
	}

	// material properties (ambient, diffuse, etc, and lighthing)
	// handled in MaterialRenderers (shader)

	// fillmode
	if (material.Wireframe)
		RasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
	else
		RasterizerDesc.FillMode = D3D11_FILL_SOLID;

	// multisample
	if (FeatureEnabled[EVDF_TEXTURE_MULTISAMPLING])
		RasterizerDesc.MultisampleEnable = true;

	// shademode (Flat or Gouraud)
	// handled in MaterialRenderers (shader)

	// lighting 
	// handled in MaterialRenderers (shader)

	// zbuffer
	DepthStencilDesc.DepthEnable = (material.ZBuffer == ECFN_NEVER) ? false : true;
	DepthStencilDesc.DepthFunc = getDepthFunction((E_COMPARISON_FUNC)material.ZBuffer);

	// zwrite
	if ( material.ZWriteEnable && (AllowZWriteOnTransparent || !material.isTransparent()))
	{
		DepthStencilDesc.DepthEnable = material.ZBuffer;
		DepthStencilDesc.DepthWriteMask = material.ZWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	}

	// back face culling
	if (material.FrontfaceCulling)
		RasterizerDesc.CullMode = D3D11_CULL_FRONT;
	else if (material.BackfaceCulling)
		RasterizerDesc.CullMode = D3D11_CULL_BACK;
	else
		RasterizerDesc.CullMode = D3D11_CULL_NONE;

	// fog
	// handled in MaterialRenderers (shader)

	// specular highlights
	// handled in MaterialRenderers (shader)

	// normalization
	// handled in MaterialRenderers (shader)

	// Color Mask
	// multiple color masks and independent blend will be handled in setRenderTarget
	BlendDesc.RenderTarget[0].RenderTargetWriteMask =
		((material.ColorMask & ECP_RED) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
		((material.ColorMask & ECP_GREEN) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
		((material.ColorMask & ECP_BLUE) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
		((material.ColorMask & ECP_ALPHA) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);

	// Polygon offset
	if (queryFeature(EVDF_POLYGON_OFFSET) && (resetAllRenderstates))
	{
		if (material.PolygonOffsetFactor)
		{
			if (material.PolygonOffsetDirection==EPO_BACK)
			{
				RasterizerDesc.SlopeScaledDepthBias = 1.f;
				RasterizerDesc.DepthBias = material.PolygonOffsetFactor;
			}
			else
			{
				RasterizerDesc.SlopeScaledDepthBias = -1.f;
				RasterizerDesc.DepthBias = -material.PolygonOffsetFactor;
			}
		}
		else
		{
			RasterizerDesc.SlopeScaledDepthBias = 0;
			RasterizerDesc.DepthBias = 0;
		}
	}

	// Anti Aliasing
	if (AlphaToCoverageSupport && (material.AntiAliasing & EAAM_ALPHA_TO_COVERAGE) )
		BlendDesc.AlphaToCoverageEnable = TRUE;
	else
		BlendDesc.AlphaToCoverageEnable = FALSE;

	// enable antialiasing
	if (Params.AntiAlias)
	{
		if (material.AntiAliasing & (EAAM_SIMPLE|EAAM_QUALITY))
			RasterizerDesc.MultisampleEnable = TRUE;
		else
			RasterizerDesc.MultisampleEnable = FALSE;

		if (material.AntiAliasing & (EAAM_LINE_SMOOTH))
			RasterizerDesc.AntialiasedLineEnable = TRUE;
		else
			RasterizerDesc.AntialiasedLineEnable = FALSE;
	}

	// thickness
	// handled in MaterialRenderers (shader)

	// texture address mode
	for (u32 st=0; st<MaxTextureUnits; ++st)
	{
		if (resetAllRenderstates || lastMaterial.TextureLayer[st].LODBias != material.TextureLayer[st].LODBias)
			SamplerDesc[st].MipLODBias = material.TextureLayer[st].LODBias * 0.125f;

		if (resetAllRenderstates || lastMaterial.TextureLayer[st].TextureWrapU != material.TextureLayer[st].TextureWrapU)		
			SamplerDesc[st].AddressU = getTextureWrapMode(material.TextureLayer[st].TextureWrapU);

		if(resetAllRenderstates || lastMaterial.TextureLayer[st].TextureWrapV != material.TextureLayer[st].TextureWrapV)
			SamplerDesc[st].AddressV = getTextureWrapMode(material.TextureLayer[st].TextureWrapV);

		// Bilinear, trilinear, and anisotropic filter
		if (material.TextureLayer[st].BilinearFilter)
		{
			SamplerDesc[st].Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			SamplerDesc[st].MaxAnisotropy = 0;
		}
		else if (material.TextureLayer[st].TrilinearFilter)
		{
			SamplerDesc[st].Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			SamplerDesc[st].MaxAnisotropy = 0;
		}
		else if (material.TextureLayer[st].AnisotropicFilter)
		{
			SamplerDesc[st].Filter = D3D11_FILTER_ANISOTROPIC;
			SamplerDesc[st].MaxAnisotropy = material.TextureLayer[st].AnisotropicFilter;
		}
	}
}

E_DRIVER_TYPE CD3D11Driver::getDriverType() const
{
	return EDT_DIRECT3D11;
}

const core::matrix4& CD3D11Driver::getTransform(E_TRANSFORMATION_STATE state) const
{
	return Matrices[state];
}

ITexture* CD3D11Driver::addRenderTargetTexture(const core::dimension2d<u32>& size,
		const io::path&name, const ECOLOR_FORMAT format)
{
	return addRenderTargetTexture(size, name, format, 1, 0, 1);
}

ITexture* CD3D11Driver::addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name, const ECOLOR_FORMAT format,
				u32 sampleCount, u32 sampleQuality, u32 arraySlices)
{
	ITexture* tex = new CD3D11Texture(this, size, name, format, arraySlices, sampleCount, sampleQuality);
	if (tex)
	{
		checkDepthBuffer(tex);
		addTexture(tex);
		tex->drop();
	}
	return tex;
}

video::ITexture* CD3D11Driver::createDeviceDependentTexture(IImage* surface, const io::path& name, void* mipmapData)
{
	return new CD3D11Texture(surface, this, TextureCreationFlags, name, 1, mipmapData);
}

IHardwareBuffer* CD3D11Driver::createHardwareBuffer(E_HARDWARE_BUFFER_TYPE type, 
													E_HARDWARE_BUFFER_ACCESS accessType, 
													u32 size, u32 flags, const void* initialData)
{
	return new CD3D11HardwareBuffer(this, type, accessType, size, flags, initialData);
}

E_VERTEX_TYPE CD3D11Driver::registerVertexType(core::array<SVertexElement>& elements)
{
	CD3D11VertexDeclaration* decl = new CD3D11VertexDeclaration(this, elements, (E_VERTEX_TYPE)declarationMap.size());
	declarationMap.insert(decl->getType(), decl);

	return decl->getType();
}

u32 CD3D11Driver::queryMultisampleLevels(ECOLOR_FORMAT format, u32 numSamples) const
{
	UINT quality = 0;
	if (SUCCEEDED(Device->CheckMultisampleQualityLevels(this->getD3DFormatFromColorFormat(format),
														4, &quality)))
	{
		return quality;
	}

	return 0;
}

void CD3D11Driver::clearZBuffer()
{
	if( CurrentDepthBuffer )
		ImmediateContext->ClearDepthStencilView( CurrentDepthBuffer, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0 );
}

IImage* CD3D11Driver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
{
	if (target != video::ERT_FRAME_BUFFER)
		return 0;

	if (format == video::ECF_UNKNOWN)
		format = video::ECF_A8R8G8B8;

	core::dimension2du size = getCurrentRenderTargetSize();

	ID3D11Texture2D* bufftex = 0;
	D3D11_MAPPED_SUBRESOURCE mappedData;
	CD3D11_TEXTURE2D_DESC tex_desc(getD3DFormatFromColorFormat(format), size.Width, size.Height, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE);

	// First, create a texture buffer
	HRESULT hr = Device->CreateTexture2D( &tex_desc, NULL, &bufftex );
	if( FAILED( hr ) )
		return NULL;

	// Copy back buffer to our texture buffer
	ID3D11Resource* backBuffer = 0;
	DefaultBackBuffer->GetResource( &backBuffer );
	ImmediateContext->CopyResource( bufftex, backBuffer );
	backBuffer->Release();

	// Get buffer data
	if( FAILED( ImmediateContext->Map( bufftex, 0, D3D11_MAP_READ_WRITE, 0, &mappedData ) ) )
		return NULL;

	IImage* newImage = new CImage( format, size );
	if(!newImage)
		return NULL;

	u32* buff = (u32*)newImage->lock();

	convertColor(mappedData.pData, format, size.getArea(), buff, video::ECF_A8R8G8B8);

	// Process data in place, handling alpha channel
	for (u32 y = 0; y < size.Height; ++y)
    {
		u8* ptr = (u8*)buff + y * mappedData.RowPitch + 3;
        for (u32 x = 0; x < size.Width; ++x)
        {
			//change blue / red channel
			u8 blue = *(ptr + 1);
			u8 red = *(ptr + 3);
			
			ptr++;
			*ptr = red;
			
			ptr += 2;

			*ptr = blue;
			ptr++;
        }
    }

	convertColor(buff, video::ECF_A8R8G8B8, size.getArea(), buff, format);

	// Unlock image and texture
	newImage->unlock();
	ImmediateContext->Unmap( bufftex, 0 );

	// Release texture buffer
	bufftex->Release();

	// Return image
	return newImage;
}

bool CD3D11Driver::setClipPlane(u32 index, const core::plane3df& plane, bool enable)
{
	if(index > 2)
		return false;

	ClipPlanes[index] = plane;
	
	enableClipPlane(index, enable);

	return true;
}

void CD3D11Driver::enableClipPlane(u32 index, bool enable)
{
	ClipPlaneEnabled[index] = enable;
}

void CD3D11Driver::getClipPlane(u32 index, core::plane3df& plane, bool& enable)
{
	plane = ClipPlanes[index];
	enable = ClipPlaneEnabled[index];
}

ECOLOR_FORMAT CD3D11Driver::getColorFormat() const
{
	return ColorFormat;
}

DXGI_FORMAT CD3D11Driver::getD3DColorFormat() const
{
	return D3DColorFormat;
}

core::dimension2du CD3D11Driver::getMaxTextureSize() const
{
	// Maximal value depends of driver type.
	switch( FeatureLevel )
	{
	case D3D_FEATURE_LEVEL_11_0:
		return core::dimension2du( 16384, 16384 );

	case D3D10_FEATURE_LEVEL_10_1:
	case D3D10_FEATURE_LEVEL_10_0:
		return core::dimension2du( 8192, 8192 );

	case D3D10_FEATURE_LEVEL_9_3:
		return core::dimension2du( 4096, 4096 );
	
	case D3D10_FEATURE_LEVEL_9_2:
	case D3D10_FEATURE_LEVEL_9_1:
		return core::dimension2du( 2048, 2048 );
	default:
		return core::dimension2du( 16384, 16384 );
	}
}

DXGI_FORMAT CD3D11Driver::getD3DFormatFromColorFormat(ECOLOR_FORMAT format) const
{
	switch(format)
	{
	case ECF_A1R5G5B5:
		return DXGI_FORMAT_B5G5R5A1_UNORM;

	case ECF_R5G6B5:
		return DXGI_FORMAT_B5G6R5_UNORM;

	case ECF_R8G8B8:
		return DXGI_FORMAT_R8G8B8A8_UNORM;

	case ECF_A8R8G8B8:
		return DXGI_FORMAT_R8G8B8A8_UNORM;

	case ECF_R16F:
		return DXGI_FORMAT_R16_FLOAT;

	case ECF_G16R16F:
		return DXGI_FORMAT_R16G16_FLOAT;

	case ECF_A16B16G16R16F:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;

	case ECF_R32F:
		return DXGI_FORMAT_R32_FLOAT;

	case ECF_G32R32F:
		return DXGI_FORMAT_R32G32_FLOAT;

	case ECF_A32B32G32R32F:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	}

	return DXGI_FORMAT_UNKNOWN;
}

ECOLOR_FORMAT CD3D11Driver::getColorFormatFromD3DFormat(DXGI_FORMAT format) const
{
	switch(format)
	{
	case DXGI_FORMAT_B5G5R5A1_UNORM:
		return ECF_A1R5G5B5;

	case DXGI_FORMAT_B5G6R5_UNORM:
		return ECF_R5G6B5;

	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return ECF_A8R8G8B8;

	case DXGI_FORMAT_R16_FLOAT:
		return ECF_R16F;

	case DXGI_FORMAT_R16G16_FLOAT :
		return ECF_G16R16F;

	case DXGI_FORMAT_R16G16B16A16_FLOAT :
		return ECF_A16B16G16R16F;

	case DXGI_FORMAT_R32_FLOAT :
		return ECF_R32F;

	case DXGI_FORMAT_R32G32_FLOAT :
		return ECF_G32R32F;

	case DXGI_FORMAT_R32G32B32A32_FLOAT :
		return ECF_A32B32G32R32F;
	}

	return (ECOLOR_FORMAT)0;
}

D3D11_PRIMITIVE_TOPOLOGY CD3D11Driver::getTopology(scene::E_PRIMITIVE_TYPE primType) const
{
	switch( primType )
	{
		case scene::EPT_POINT_SPRITES:
		case scene::EPT_POINTS:
			return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;

		case scene::EPT_LINE_STRIP:
			return  D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;

		case scene::EPT_LINE_LOOP:
		case scene::EPT_LINES:
			return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;

		case scene::EPT_TRIANGLE_STRIP:
			return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

		case scene::EPT_TRIANGLES:
			return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		case scene::EPT_TRIANGLE_FAN:
			return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

		default:
			return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	}
}

u32 CD3D11Driver::getBitsPerPixel(DXGI_FORMAT format) const
{
	switch( format )
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 64;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 32;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return 16;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 8;

    case DXGI_FORMAT_R1_UNORM:
        return 1;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return 4;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 8;

    default:
        return 0;
	}
}

u32 CD3D11Driver::getNumberOfComponents(DXGI_FORMAT format) const
{
	switch( format )
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 4;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
        return 3;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
        return 2;

	case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
	case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R1_UNORM:
		return 1;
    
    default:
        return 0;
	}
}

u32 CD3D11Driver::getIndexAmount(scene::E_PRIMITIVE_TYPE primType, u32 primitiveCount) const
{
	switch( primType )
	{
		case scene::EPT_LINE_STRIP:
			return primitiveCount * 2;

		case scene::EPT_LINE_LOOP:
		case scene::EPT_LINES:
			return primitiveCount * 2;

		case scene::EPT_TRIANGLE_STRIP:
			return primitiveCount + 2;

		case scene::EPT_TRIANGLES:
			return primitiveCount * 3;

		case scene::EPT_TRIANGLE_FAN:
			return primitiveCount * 3;

		default:
			return 0;
	}
}

D3D11_TEXTURE_ADDRESS_MODE CD3D11Driver::getTextureWrapMode(const u8 clamp)
{
	switch (clamp)
	{
		case ETC_REPEAT:
			return D3D11_TEXTURE_ADDRESS_WRAP;
		case ETC_CLAMP:
		case ETC_CLAMP_TO_EDGE:
			return D3D11_TEXTURE_ADDRESS_CLAMP;
		case ETC_MIRROR:
			return D3D11_TEXTURE_ADDRESS_MIRROR;
		case ETC_CLAMP_TO_BORDER:
			return D3D11_TEXTURE_ADDRESS_BORDER;
		case ETC_MIRROR_CLAMP:
		case ETC_MIRROR_CLAMP_TO_EDGE:
		case ETC_MIRROR_CLAMP_TO_BORDER:
			return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
		default:
			return D3D11_TEXTURE_ADDRESS_WRAP;
	}
}

D3D11_COMPARISON_FUNC CD3D11Driver::getDepthFunction(E_COMPARISON_FUNC func) const
{
	switch (func)
	{
	case ECFN_LESSEQUAL:
		return D3D11_COMPARISON_LESS_EQUAL;
	case ECFN_EQUAL:
		return D3D11_COMPARISON_EQUAL;
	case ECFN_LESS:
		return D3D11_COMPARISON_LESS;
	case ECFN_NOTEQUAL:
		return D3D11_COMPARISON_NOT_EQUAL;
	case ECFN_GREATEREQUAL:
		return D3D11_COMPARISON_GREATER_EQUAL;
	case ECFN_GREATER:
		return D3D11_COMPARISON_GREATER;
	case ECFN_ALWAYS:
		return D3D11_COMPARISON_ALWAYS;
	case ECFN_NEVER:
	default:
		return D3D11_COMPARISON_NEVER;
	}
}

D3D11_COLOR_WRITE_ENABLE CD3D11Driver::getColorWriteEnable(E_COLOR_PLANE plane) const
{
	return (D3D11_COLOR_WRITE_ENABLE)
			(
				( (plane & ECP_RED) ? 	D3D11_COLOR_WRITE_ENABLE_RED : 0) 		|
				( (plane & ECP_GREEN) ? 	D3D11_COLOR_WRITE_ENABLE_GREEN : 0) 	|
				( (plane & ECP_BLUE) ? 	D3D11_COLOR_WRITE_ENABLE_BLUE : 0) 		|
				( (plane & ECP_ALPHA) ? 	D3D11_COLOR_WRITE_ENABLE_ALPHA : 0)
			);
}

DXGI_FORMAT CD3D11Driver::getIndexType(E_INDEX_TYPE iType) const
{
	return (iType == video::EIT_16BIT) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

CD3D11VertexDeclaration* CD3D11Driver::getVertexDeclaration(E_VERTEX_TYPE vType) const
{
	DeclarationNode declNode = declarationMap.find(vType);
	if (declNode)
		return declNode->getValue();
	
	return 0;
}

bool CD3D11Driver::querySupportForColorFormat(DXGI_FORMAT format, D3D11_FORMAT_SUPPORT support)
{
	UINT values = 0;
	if( FAILED( Device->CheckFormatSupport(format, &values) ) )
		return false;

	if( values && support )
		return true;

	return false;
}

void CD3D11Driver::checkDepthBuffer(ITexture* tex)
{
	if (!tex)
		return;

	const core::dimension2du optSize = tex->getSize().getOptimalSize(
			!queryFeature(EVDF_TEXTURE_NPOT),
			!queryFeature(EVDF_TEXTURE_NSQUARE), true);
	SDepthSurface11* depth=0;
	core::dimension2du destSize(0x7fffffff, 0x7fffffff);
	for (u32 i=0; i<DepthBuffers.size(); ++i)
	{
		if ((DepthBuffers[i]->Size.Width>=optSize.Width) &&
			(DepthBuffers[i]->Size.Height>=optSize.Height))
		{
			if ((DepthBuffers[i]->Size.Width<destSize.Width) &&
				(DepthBuffers[i]->Size.Height<destSize.Height))
			{
				depth = DepthBuffers[i];
				destSize=DepthBuffers[i]->Size;
			}
		}
	}
	
	// Create a depth buffer for this texture
	if (!depth)
	{
		// create depth buffer
		DepthBuffers.push_back(new SDepthSurface11());
		DepthBuffers.getLast()->Surface = createDepthStencilView(optSize);
		if ( DepthBuffers.getLast()->Surface )
		{
			depth=DepthBuffers.getLast();
			depth->Size.set(optSize.Width, optSize.Height);
		}
		else
		{
			char buffer[128];
			sprintf(buffer,"Could not create DepthBuffer of %ix%i",optSize.Width,optSize.Height);
			os::Printer::log(buffer,ELL_ERROR);
			DepthBuffers.erase(DepthBuffers.size()-1);
		}
	}
	else
		depth->grab();

	static_cast<CD3D11Texture*>(tex)->DepthSurface=depth;
}

void CD3D11Driver::removeDepthSurface(SDepthSurface11* depth)
{
	for (u32 i=0; i<DepthBuffers.size(); ++i)
	{
		if (DepthBuffers[i]==depth)
		{
			DepthBuffers.erase(i);
			return;
		}
	}
}

bool CD3D11Driver::uploadVertexData(const void* vertices, u32 vertexCount,
									const void* indexList, u32 indexCount,
									E_VERTEX_TYPE vType, E_INDEX_TYPE iType)
{
	// Parse information about buffers
	const u32 vertexStride = getVertexPitchFromType(vType);
	const u32 indexStride = iType == video::EIT_16BIT ? 2 : 4;
	const DXGI_FORMAT indexFormat = iType == video::EIT_16BIT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	const UINT offset = 0;

	// reallocated if needed
	if( !reallocateDynamicBuffers( vertexCount * vertexStride, indexCount * indexStride ) )
		return false;

	HRESULT hr = S_OK;
	D3D11_MAPPED_SUBRESOURCE mappedData;
	ZeroMemory(&mappedData, sizeof(D3D11_MAPPED_SUBRESOURCE));

	if(vertices)
	{		
		hr = ImmediateContext->Map( DynVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedData );
		if(SUCCEEDED(hr))
		{
			memcpy(mappedData.pData, vertices, vertexCount * vertexStride);
			ImmediateContext->Unmap( DynVertexBuffer, 0 );
		}
		else
		{
			os::Printer::log("Error, could not upload dynamic vertex data to GPU", ELL_ERROR);
			return false;
		}

		// set vertex buffer
		ImmediateContext->IASetVertexBuffers( 0, 1, &DynVertexBuffer, &vertexStride, &offset );
	}

	if(indexList)
	{
		hr = ImmediateContext->Map( DynIndexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedData );
		if(SUCCEEDED(hr))
		{
			memcpy(mappedData.pData, indexList, indexCount * indexStride);
			ImmediateContext->Unmap( DynIndexBuffer, 0 );
		}
		else
		{
			os::Printer::log("Error, could not upload dynamic index data to GPU", ELL_ERROR);
			return false;
		}

		// set index buffer
		ImmediateContext->IASetIndexBuffer( DynIndexBuffer, indexFormat, 0 );
	}

	return true;
}

bool CD3D11Driver::reallocateDynamicBuffers( u32 vertexBufferSize, u32 indexBufferSize )
{
	HRESULT hr = S_OK;
	D3D11_BUFFER_DESC desc;
	::ZeroMemory(&desc, sizeof(D3D11_BUFFER_DESC));

	if( !DynVertexBuffer || DynVertexBufferSize < vertexBufferSize )
	{
		// Release old buffer if small
		if( DynVertexBufferSize < vertexBufferSize )
			SAFE_RELEASE(DynVertexBuffer);

		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth = vertexBufferSize;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = 0;
		hr = Device->CreateBuffer( &desc, NULL, &DynVertexBuffer);
		if(FAILED(hr))
		{
			os::Printer::log("Error, failed to create dynamic vertex buffer", ELL_ERROR);
			return false;
		}

		DynVertexBufferSize = vertexBufferSize;
	}

	if( !DynIndexBuffer || DynIndexBufferSize < indexBufferSize )
	{
		// Release old buffer if small
		if( DynIndexBufferSize < indexBufferSize ) 
			SAFE_RELEASE(DynIndexBuffer);

		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.ByteWidth = indexBufferSize;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = 0;
		hr = Device->CreateBuffer( &desc, NULL, &DynIndexBuffer);
		if(FAILED(hr))
		{
			os::Printer::log("Error, failed to create dynamic index buffer", ELL_ERROR);
			return false;
		}

		DynIndexBufferSize = indexBufferSize;
	}

	return true;
}

ID3D11DepthStencilView* CD3D11Driver::createDepthStencilView(core::dimension2d<u32> size)
{
	// check stencil buffer format
	if( DepthStencilFormat == DXGI_FORMAT_UNKNOWN )
	{
		DepthStencilFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		UINT formatSupport = 0;
		if(Params.Stencilbuffer)
		{
			Device->CheckFormatSupport( DepthStencilFormat, &formatSupport );
			if( ( formatSupport && D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ) == 0 )
			{
				DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
				Device->CheckFormatSupport( DepthStencilFormat, &formatSupport );
				if( ( formatSupport && D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ) == 0 )
				{
					os::Printer::log("Device does not support stencilbuffer, disabling stencil buffer.", ELL_WARNING);
					Params.Stencilbuffer = false;
				}
			}		
		}
		if (!Params.Stencilbuffer)	// do not use else here to cope with flag change in previous block
		{
			DepthStencilFormat = DXGI_FORMAT_D32_FLOAT;
			Device->CheckFormatSupport( DepthStencilFormat, &formatSupport );
			if( ( formatSupport && D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ) == 0 )
			{
				DepthStencilFormat = DXGI_FORMAT_D16_UNORM;
				Device->CheckFormatSupport( DepthStencilFormat, &formatSupport );
				if( ( formatSupport && D3D11_FORMAT_SUPPORT_DEPTH_STENCIL ) == 0 )
				{
					os::Printer::log("Device does not support required depth buffer.", ELL_WARNING);
					return NULL;
				}
			}
		}
	}

	// create depth buffer
	ID3D11DepthStencilView* dsView = NULL;
	ID3D11Texture2D* depthTexture = NULL;
	D3D11_TEXTURE2D_DESC dsTexDesc;
	::ZeroMemory( &dsTexDesc, sizeof( dsTexDesc ) );
	dsTexDesc.ArraySize = 1;
	dsTexDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	dsTexDesc.CPUAccessFlags = 0;
	dsTexDesc.Format = DepthStencilFormat;
	dsTexDesc.MipLevels = 1;
	dsTexDesc.MiscFlags = 0;
	dsTexDesc.Usage = D3D11_USAGE_DEFAULT;
	dsTexDesc.SampleDesc.Count = 1;
	dsTexDesc.SampleDesc.Quality = 0;
	dsTexDesc.Width = (UINT)size.Width;
	dsTexDesc.Height = (UINT)size.Height;
	HRESULT hr = Device->CreateTexture2D( &dsTexDesc, NULL, &depthTexture );
	if(FAILED(hr))
	{
		os::Printer::log("Error, could not create depth texture.", ELL_WARNING);
		return NULL;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC dsDesc;
	::ZeroMemory( &dsDesc, sizeof( dsDesc ) );
	dsDesc.Format = DepthStencilFormat;
	dsDesc.Flags = 0;
	dsDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	dsDesc.Texture2D.MipSlice = 0;
	hr = Device->CreateDepthStencilView( depthTexture, &dsDesc, &dsView );
	depthTexture->Release();
	if(FAILED(hr))
	{
		os::Printer::log("Error, could not create depth stencil view.", ELL_WARNING);
		return NULL;
	}

	return dsView;
}

void CD3D11Driver::reset()
{
	u32 i;
	os::Printer::log("Resetting D3D11 device.", ELL_INFORMATION);

	for (i=0; i<Textures.size(); ++i)
	{
		if (Textures[i].Surface->isRenderTarget())
		{
			ID3D11Resource* tex = ((CD3D11Texture*)(Textures[i].Surface))->getTextureResource();
			if (tex)
				tex->Release();
		}
	}
	for (i=0; i<DepthBuffers.size(); ++i)
	{
		if (DepthBuffers[i]->Surface)
			DepthBuffers[i]->Surface->Release();
	}

	for (i=0; i<OcclusionQueries.size(); ++i)
	{
		if (OcclusionQueries[i].PID)
		{
			reinterpret_cast<ID3D11Query*>(OcclusionQueries[i].PID)->Release();
			OcclusionQueries[i].PID=0;
		}
	}

	HRESULT hr = S_OK;

	// unbind render targets
	ID3D11RenderTargetView* views[] = { NULL };
	ImmediateContext->OMSetRenderTargets(1, views, NULL);

	SAFE_RELEASE(DefaultBackBuffer);
	SAFE_RELEASE(DefaultDepthBuffer);

	// If fullscreen, do it
	if(Params.Fullscreen)
	{
		SwapChain->SetFullscreenState( TRUE, Output );
	}

	// resize targets. Shows error when working with Parallel NSight
#ifdef _DEBUG
	hr = SwapChain->ResizeBuffers( 1, ScreenSize.Width, ScreenSize.Height, D3DColorFormat, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH );
	if(FAILED(hr))
	{
		os::Printer::log("Error resizing back buffer", ELL_ERROR);
		return;
	}
#endif

	// Get default render target
	ID3D11Texture2D* backBuffer = NULL;
	hr = SwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), reinterpret_cast<void**>( &backBuffer ) );
	if( FAILED(hr))
	{
		os::Printer::log("Error, could not get back buffer.", ELL_ERROR);
		return;
	}
	hr = Device->CreateRenderTargetView( backBuffer, NULL, &DefaultBackBuffer );
	if( FAILED(hr))
	{
		os::Printer::log("Error, could not create render target view.", ELL_ERROR);
		return;
	}
	backBuffer->Release();

	// create depth buffer
	DefaultDepthBuffer = createDepthStencilView(ScreenSize);

	// Set render targets
	ImmediateContext->OMSetRenderTargets( 1, &DefaultBackBuffer, DefaultDepthBuffer );

	LastVertexType = EVT_STANDARD;

	ResetRenderStates = true;

	for (u32 i=0; i<MATERIAL_MAX_TEXTURES; ++i)
		CurrentTexture[i] = 0;

	removeAllHardwareBuffers();
	removeAllOcclusionQueries();

	setFog(FogColor, FogType, FogStart, FogEnd, FogDensity, PixelFog, RangeFog);
	setAmbientLight(AmbientLight);
}

// returns the current size of the screen or rendertarget
const core::dimension2d<u32>& CD3D11Driver::getCurrentRenderTargetSize() const
{
	if ( CurrentRendertargetSize.Width == 0 )
		return ScreenSize;
	else
		return CurrentRendertargetSize;
}

////////////// IGPUProgrammingServices methods start ////////////////////////////////////////////

s32 CD3D11Driver::addHighLevelShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
			IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	E_VERTEX_TYPE vertexTypeOut = EVT_STANDARD;
	s32 id = 0;
	CD3D11CustomMaterialRenderer* rend = 
			new CD3D11CustomMaterialRenderer(Device, this, id,
											 vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
											 pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
											 geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
											 inType, outType, verticesOut, vertexTypeOut,
											 callback, getMaterialRenderer(baseMaterial), userData);
	rend->drop();

	return id;
}

//! Adds a new material renderer to the VideoDriver, using pixel and/or
//! vertex shaders to render geometry.
s32 CD3D11Driver::addShaderMaterial(const c8* vertexShaderProgram,
								   const c8* pixelShaderProgram,
								   IShaderConstantSetCallBack* callback,
								   E_MATERIAL_TYPE baseMaterial,
								   s32 userData)
{
	s32 nr = -1;
	/*CD3D11ShaderMaterialRenderer* r = new CD3D11ShaderMaterialRenderer(
		Device, this, nr, vertexShaderProgram, pixelShaderProgram,
		callback, getMaterialRenderer(baseMaterial), userData);

	r->drop();*/
	return nr;
}

////////////// IGPUProgrammingServices methods end ////////////////////////////////////////////

////////////// IMaterialRenderer methods start ////////////////////////////////////////////

//! Get a vertex shader constant index.
s32 CD3D11Driver::getVertexShaderConstantID(const c8* name)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->getVariableID(true, name);
	}

	return -1;
}

//! Get a pixel shader constant index.
s32 CD3D11Driver::getPixelShaderConstantID(const c8* name)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->getVariableID(false, name);
	}

	return -1;
}


//! Sets a vertex shader constant.
void CD3D11Driver::setVertexShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
{
	os::Printer::log("\"setVertexShaderConstant\" with offset is not supported", ELL_ERROR);
}


//! Sets a pixel shader constant.
void CD3D11Driver::setPixelShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
{
	os::Printer::log("\"setPixelShaderConstant\" with offset is not supported", ELL_ERROR);
}


//! Sets a constant for the vertex shader based on an index.
bool CD3D11Driver::setVertexShaderConstant(s32 index, const f32* floats, int count)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->setVariable(true, index, floats, count);
	}

	return false;
}


//! Int interface for the above.
bool CD3D11Driver::setVertexShaderConstant(s32 index, const s32* ints, int count)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->setVariable(true, index, ints, count);
	}

	return false;
}


//! Sets a constant for the pixel shader based on an index.
bool CD3D11Driver::setPixelShaderConstant(s32 index, const f32* floats, int count)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->setVariable(false, index, floats, count);
	}

	return false;
}


//! Int interface for the above.
bool CD3D11Driver::setPixelShaderConstant(s32 index, const s32* ints, int count)
{
	if (Material.MaterialType >= 0 && Material.MaterialType < (s32)MaterialRenderers.size())
	{
		CD3D11MaterialRenderer* r = (CD3D11MaterialRenderer*)MaterialRenderers[Material.MaterialType].Renderer;
		return r->setVariable(false, index, ints, count);
	}

	return false;
}

bool CD3D11Driver::setStreamOutputBuffer(IHardwareBuffer* buffer)
{
	ID3D11Buffer* buffers = 0;
	UINT offset = 0;

	// If buffer is null, remove from pipeline
	if (!buffer)
	{
		ImmediateContext->SOSetTargets(1, &buffers, &offset);
		return true;
	}

	// Validate buffer
	if (buffer->getDriverType() != EDT_DIRECT3D11)
	{
		os::Printer::log("Fatal Error: Tried to set a buffer not owned by this driver.", ELL_ERROR);
		return false;
	}
	if (buffer->getType() != EHBT_STREAM_OUTPUT)
	{
		os::Printer::log("Fatal Error: Tried to set a buffer that is not for stream output.", ELL_ERROR);
		return false;
	}

	// Set stream output buffer
	buffers = static_cast<CD3D11HardwareBuffer*>(buffer)->getBufferResource();
	ImmediateContext->SOSetTargets(1, &buffers, &offset);

	return true;
}

IVideoDriver* CD3D11Driver::getVideoDriver()
{
	return this;
}

////////////// IMaterialRenderer methods end ////////////////////////////////////////////

} // end namespace video
} // end namespace irr

#endif


namespace irr
{
namespace video
{

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_
//! creates a video driver
IVideoDriver* createDirectX11Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
{
	CD3D11Driver* dx11 = new CD3D11Driver(params,io, window);
	if (!dx11->initDriver(window, false))
	{
		dx11->drop();
		dx11 = 0;
	}

	return dx11;
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_11_

} // end namespace video
} // end namespace irr