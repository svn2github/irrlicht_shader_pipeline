
#ifndef __C_DIRECTX11_TEXTURE_H_INCLUDED__
#define __C_DIRECTX11_TEXTURE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "ITexture.h"
#include "IImage.h"
#include <d3d11.h>


namespace irr
{
namespace video
{

class CD3D11Driver;
// forward declaration for RTT depth buffer handling
struct SDepthSurface11;

class CD3D11Texture : public ITexture
{
public:

	//! constructor
	CD3D11Texture(IImage* image, CD3D11Driver* driver,
			u32 flags, const io::path& name, u32 arraySlices = 1, void* mipmapData=0);

	//! rendertarget constructor
	CD3D11Texture(CD3D11Driver* driver, const core::dimension2d<u32>& size, const io::path& name,
		const ECOLOR_FORMAT format = ECF_UNKNOWN, u32 arraySlices = 1, 
		u32 sampleCount = 1, u32 sampleQuality = 0 );

	//! destructor
	virtual ~CD3D11Texture();

	//! lock function
	virtual void* lock(E_TEXTURE_LOCK_MODE mode=ETLM_READ_WRITE, u32 mipmapLevel=0);
	virtual void* lock(bool readOnly , u32 mipmapLevel=0, u32 arraySlice = 0);

	//! unlock function
	virtual void unlock();

	//! Returns original size of the texture.
	virtual const core::dimension2d<u32>& getOriginalSize() const;

	//! Returns (=size) of the texture.
	virtual const core::dimension2d<u32>& getSize() const;

	//! returns driver type of texture (=the driver, who created the texture)
	virtual E_DRIVER_TYPE getDriverType() const;

	//! returns color format of texture
	virtual ECOLOR_FORMAT getColorFormat() const;

	virtual u32 getPitch() const;

	//! returns if texture has mipmap levels
	bool hasMipMaps() const;

	virtual u32 getNumberOfArraySlices() const;

	//! Regenerates the mip map levels of the texture. Useful after locking and
	//! modifying the texture
	virtual void regenerateMipMapLevels(void* mipmapData=0);

	//! returns if it is a render target
	virtual bool isRenderTarget() const;

public:
	//! return texture resource
	ID3D11Resource* getTextureResource() const;

	//! return render target view
	ID3D11RenderTargetView* getRenderTargetView() const;

	//! return shader resource view
	ID3D11ShaderResourceView* getShaderResourceView() const;

private:
	friend class CD3D11Driver;

	ID3D11Device* Device;
	ID3D11DeviceContext* ImmediateContext;
	ID3D11Texture2D* Texture;
	ID3D11RenderTargetView* RTView;
	ID3D11ShaderResourceView* SRView;
	D3D11_RESOURCE_DIMENSION TextureDimension;
	D3D11_MAP LastMapDirection;
	
	CD3D11Driver* Driver;
	SDepthSurface11* DepthSurface;
	core::dimension2d<u32> TextureSize;
	core::dimension2d<u32> ImageSize;
	s32 Pitch;
	u32 NumberOfMipLevels;
	u32 NumberOfArraySlices;
	ECOLOR_FORMAT ColorFormat;
	u32 SampleCount;
	u32 SampleQuality;

	ID3D11Texture2D* TextureBuffer;		// staging texture used for lock/unlock
	u32 MipLevelLocked;
	u32 ArraySliceLocked;

	bool HasMipMaps;
	bool HardwareMipMaps;
	bool IsRenderTarget;

	//! creates hardware render target
	void createRenderTarget(const ECOLOR_FORMAT format = ECF_UNKNOWN);

	//! creates the hardware texture
	bool createTexture(u32 flags, IImage * image);

	//! copies the image to the texture
	bool copyTexture(IImage* image);

	//! set Pitch based on the d3d format
	void setPitch(DXGI_FORMAT d3dformat);

	//! create texture buffer needed for lock/unlock
	bool createTextureBuffer();

	//! create views to bound texture to pipeline
	bool createViews();
};

}
}

#endif
#endif