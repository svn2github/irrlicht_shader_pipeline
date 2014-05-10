// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "IrrCompileConfig.h"
#include "CSoftwareDriver.h"

#ifdef _IRR_COMPILE_WITH_SOFTWARE_

#include "CSoftwareTexture.h"
#include "CBlit.h"
#include "os.h"
#include "S3DVertex.h"

namespace irr
{
namespace video
{


//! constructor
CSoftwareDriver::CSoftwareDriver(const core::dimension2d<u32>& windowSize, bool fullscreen, io::IFileSystem* io, video::IImagePresenter* presenter)
: CNullDriver(io, windowSize), BackBuffer(0), Presenter(presenter), WindowId(0),
	SceneSourceRect(0), RenderTargetTexture(0), RenderTargetSurface(0),
	CurrentTriangleRenderer(0), ZBuffer(0), Texture(0)
{
	#ifdef _DEBUG
	setDebugName("CSoftwareDriver");
	#endif

	// create backbuffer

	BackBuffer = new CImage(ECF_A1R5G5B5, windowSize);
	if (BackBuffer)
	{
		BackBuffer->fill(SColor(0));

		// create z buffer
		ZBuffer = video::createZBuffer(BackBuffer->getDimension());
	}

	DriverAttributes->setAttribute("MaxTextures", 1);
	DriverAttributes->setAttribute("MaxIndices", 1<<16);
	DriverAttributes->setAttribute("MaxTextureSize", 1024);
	DriverAttributes->setAttribute("Version", 1);

	createVertexDescriptors();
	// create triangle renderers

	TriangleRenderers[ETR_FLAT] = createTriangleRendererFlat(ZBuffer);
	TriangleRenderers[ETR_FLAT_WIRE] = createTriangleRendererFlatWire(ZBuffer);
	TriangleRenderers[ETR_GOURAUD] = createTriangleRendererGouraud(ZBuffer);
	TriangleRenderers[ETR_GOURAUD_WIRE] = createTriangleRendererGouraudWire(ZBuffer);
	TriangleRenderers[ETR_TEXTURE_FLAT] = createTriangleRendererTextureFlat(ZBuffer);
	TriangleRenderers[ETR_TEXTURE_FLAT_WIRE] = createTriangleRendererTextureFlatWire(ZBuffer);
	TriangleRenderers[ETR_TEXTURE_GOURAUD] = createTriangleRendererTextureGouraud(ZBuffer);
	TriangleRenderers[ETR_TEXTURE_GOURAUD_WIRE] = createTriangleRendererTextureGouraudWire(ZBuffer);
	TriangleRenderers[ETR_TEXTURE_GOURAUD_NOZ] = createTriangleRendererTextureGouraudNoZ();
	TriangleRenderers[ETR_TEXTURE_GOURAUD_ADD] = createTriangleRendererTextureGouraudAdd(ZBuffer);

	// select render target

	setRenderTarget(BackBuffer);

	// select the right renderer

	selectRightTriangleRenderer();
}



//! destructor
CSoftwareDriver::~CSoftwareDriver()
{
	// delete Backbuffer
	if (BackBuffer)
		BackBuffer->drop();

	// delete triangle renderers

	for (s32 i=0; i<ETR_COUNT; ++i)
		if (TriangleRenderers[i])
			TriangleRenderers[i]->drop();

	// delete zbuffer

	if (ZBuffer)
		ZBuffer->drop();

	// delete current texture

	if (Texture)
		Texture->drop();

	if (RenderTargetTexture)
		RenderTargetTexture->drop();

	if (RenderTargetSurface)
		RenderTargetSurface->drop();
}



//! switches to a triangle renderer
void CSoftwareDriver::switchToTriangleRenderer(ETriangleRenderer renderer)
{
	video::IImage* s = 0;
	if (Texture)
		s = ((CSoftwareTexture*)Texture)->getTexture();

	CurrentTriangleRenderer = TriangleRenderers[renderer];
	CurrentTriangleRenderer->setBackfaceCulling(Material.BackfaceCulling == true);
	CurrentTriangleRenderer->setTexture(s);
	CurrentTriangleRenderer->setRenderTarget(RenderTargetSurface, ViewPort);
}


//! void selects the right triangle renderer based on the render states.
void CSoftwareDriver::selectRightTriangleRenderer()
{

	ETriangleRenderer renderer = ETR_FLAT;

	if (Texture)
	{
		if (!Material.GouraudShading)
			renderer = (!Material.Wireframe) ? ETR_TEXTURE_FLAT : ETR_TEXTURE_FLAT_WIRE;
		else
		{
			if (Material.Wireframe)
				renderer = ETR_TEXTURE_GOURAUD_WIRE;
			else
			{
				if (Material.MaterialType == EMT_TRANSPARENT_ADD_COLOR ||
						Material.MaterialType == EMT_TRANSPARENT_ALPHA_CHANNEL ||
						Material.MaterialType == EMT_TRANSPARENT_VERTEX_ALPHA)
				{
					// simply draw all transparent stuff with the same renderer. at
					// least it is transparent then.
					renderer = ETR_TEXTURE_GOURAUD_ADD;
				}
				else
				if ((Material.ZBuffer==ECFN_DISABLED) && !Material.ZWriteEnable)
					renderer = ETR_TEXTURE_GOURAUD_NOZ;
				else
				{
					renderer = ETR_TEXTURE_GOURAUD;
				}
			}
		}
	}
	else
	{
		if (!Material.GouraudShading)
			renderer = (!Material.Wireframe) ? ETR_FLAT : ETR_FLAT_WIRE;
		else
			renderer = (!Material.Wireframe) ? ETR_GOURAUD : ETR_GOURAUD_WIRE;
	}

	switchToTriangleRenderer(renderer);
}


//! queries the features of the driver, returns true if feature is available
bool CSoftwareDriver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
{
	switch (feature)
	{
	case EVDF_RENDER_TO_TARGET:
	case EVDF_TEXTURE_NSQUARE:
		return FeatureEnabled[feature];
	default:
		return false;
	};
}


//! sets transformation
void CSoftwareDriver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
{
	TransformationMatrix[state] = mat;
}


//! sets the current Texture
bool CSoftwareDriver::setActiveTexture(u32 stage, video::ITexture* texture)
{
	if (texture && texture->getDriverType() != EDT_SOFTWARE)
	{
		os::Printer::log("Fatal Error: Tried to set a texture not owned by this driver.", ELL_ERROR);
		return false;
	}

	if (Texture)
		Texture->drop();

	Texture = texture;

	if (Texture)
		Texture->grab();

	selectRightTriangleRenderer();
	return true;
}


//! sets a material
void CSoftwareDriver::setMaterial(const SMaterial& material)
{
	Material = material;
	OverrideMaterial.apply(Material);

	for (u32 i = 0; i < 1; ++i)
	{
		setActiveTexture(i, Material.getTexture(i));
		setTransform ((E_TRANSFORMATION_STATE) ( ETS_TEXTURE_0 + i ),
				material.getTextureMatrix(i));
	}
}


//! clears the zbuffer
bool CSoftwareDriver::beginScene(bool backBuffer, bool zBuffer, SColor color,
		const SExposedVideoData& videoData, core::rect<s32>* sourceRect)
{
	CNullDriver::beginScene(backBuffer, zBuffer, color, videoData, sourceRect);
	WindowId=videoData.D3D9.HWnd;
	SceneSourceRect = sourceRect;

	if (backBuffer && BackBuffer)
		BackBuffer->fill(color);

	if (ZBuffer && zBuffer)
		ZBuffer->clear();

	return true;
}


//! presents the rendered scene on the screen, returns false if failed
bool CSoftwareDriver::endScene()
{
	CNullDriver::endScene();

	return Presenter->present(BackBuffer, WindowId, SceneSourceRect);
}


//! returns a device dependent texture from a software surface (IImage)
//! THIS METHOD HAS TO BE OVERRIDDEN BY DERIVED DRIVERS WITH OWN TEXTURES
ITexture* CSoftwareDriver::createDeviceDependentTexture(IImage* surface, const io::path& name, void* mipmapData)
{
	return new CSoftwareTexture(surface, name, false, mipmapData);
}


//! sets a render target
bool CSoftwareDriver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer,
								bool clearZBuffer, SColor color)
{
	if (texture && texture->getDriverType() != EDT_SOFTWARE)
	{
		os::Printer::log("Fatal Error: Tried to set a texture not owned by this driver.", ELL_ERROR);
		return false;
	}

	if (RenderTargetTexture)
		RenderTargetTexture->drop();

	RenderTargetTexture = texture;

	if (RenderTargetTexture)
	{
		RenderTargetTexture->grab();
		setRenderTarget(((CSoftwareTexture*)RenderTargetTexture)->getTexture());
	}
	else
	{
		setRenderTarget(BackBuffer);
	}

	if (RenderTargetSurface && (clearBackBuffer || clearZBuffer))
	{
		if (clearZBuffer)
			ZBuffer->clear();

		if (clearBackBuffer)
			RenderTargetSurface->fill(color);
	}

	return true;
}


//! sets a render target
void CSoftwareDriver::setRenderTarget(video::CImage* image)
{
	if (RenderTargetSurface)
		RenderTargetSurface->drop();

	RenderTargetSurface = image;
	RenderTargetSize.Width = 0;
	RenderTargetSize.Height = 0;
	Render2DTranslation.X = 0;
	Render2DTranslation.Y = 0;

	if (RenderTargetSurface)
	{
		RenderTargetSurface->grab();
		RenderTargetSize = RenderTargetSurface->getDimension();
	}

	setViewPort(core::rect<s32>(0,0,RenderTargetSize.Width,RenderTargetSize.Height));

	if (ZBuffer)
		ZBuffer->setSize(RenderTargetSize);
}


//! sets a viewport
void CSoftwareDriver::setViewPort(const core::rect<s32>& area)
{
	ViewPort = area;

	//TODO: the clipping is not correct, because the projection is affected.
	// to correct this, ViewPortSize and Render2DTranslation will have to be corrected.
	core::rect<s32> rendert(0,0,RenderTargetSize.Width,RenderTargetSize.Height);
	ViewPort.clipAgainst(rendert);

	ViewPortSize = core::dimension2du(ViewPort.getSize());
	Render2DTranslation.X = (ViewPortSize.Width / 2) + ViewPort.UpperLeftCorner.X;
	Render2DTranslation.Y = ViewPort.UpperLeftCorner.Y + ViewPortSize.Height - (ViewPortSize.Height / 2);// + ViewPort.UpperLeftCorner.Y;

	if (CurrentTriangleRenderer)
		CurrentTriangleRenderer->setRenderTarget(RenderTargetSurface, ViewPort);
}


void CSoftwareDriver::drawVertexPrimitiveList(scene::IVertexBuffer* vertexBuffer, scene::IIndexBuffer* indexBuffer,
	IVertexDescriptor* descriptor, u32 primitiveCount, scene::E_PRIMITIVE_TYPE pType)
{
	if (!vertexBuffer || !indexBuffer || !descriptor || primitiveCount == 0)
		return;
		
	if (indexBuffer->getType() == EIT_32BIT)
	{
		os::Printer::log("Software driver can not render 32bit buffers", ELL_ERROR);
		return;
	}

	video::IVertexAttribute* AttributeP = descriptor->getAttributeBySemantic(video::EVAS_POSITION);
	video::IVertexAttribute* AttributeC = descriptor->getAttributeBySemantic(video::EVAS_COLOR);

	if (!AttributeP)
		return;

	scene::CIndexBuffer NewBuffer(EIT_16BIT);
	scene::IIndexBuffer* IndexPointer = 0;

	u8* vertexData = static_cast<u8*>(vertexBuffer->getVertices());
	const u32 vertexSize = vertexBuffer->getVertexSize();

	u8* OffsetP = vertexData + AttributeP->getOffset();
	u8* OffsetC = (AttributeC) ? vertexData + AttributeC->getOffset() : 0;

	switch (pType)
	{
	case scene::EPT_LINE_STRIP:
		{
			for (u32 i=0; i < primitiveCount-1; ++i)
			{
				core::vector3df* PositionA = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(i);
				core::vector3df* PositionB = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(i + 1);

				if(AttributeC)
				{
					SColor* Color = (SColor*)OffsetC + vertexSize * indexBuffer->getIndex(i);

					draw3DLine(*PositionA, *PositionB, *Color);
				}
				else
					draw3DLine(*PositionA, *PositionB);
			}
		}
		return;
	case scene::EPT_LINE_LOOP:
		drawVertexPrimitiveList(vertexBuffer, indexBuffer, descriptor, primitiveCount, scene::EPT_LINE_STRIP);
		{
			core::vector3df* PositionA = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(primitiveCount - 1);
			core::vector3df* PositionB = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(0);

			if(AttributeC)
			{
				SColor* Color = (SColor*)OffsetC + vertexSize * indexBuffer->getIndex(primitiveCount-1);

				draw3DLine(*PositionA, *PositionB, *Color);
			}
			else
				draw3DLine(*PositionA, *PositionB);
		}
		return;
	case scene::EPT_LINES:
		{
			for (u32 i=0; i < 2*primitiveCount; i+=2)
			{
				core::vector3df* PositionA = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(i);
				core::vector3df* PositionB = (core::vector3df*)OffsetP + vertexSize * indexBuffer->getIndex(i + 1);

				if(AttributeC)
				{
					SColor* Color = (SColor*)OffsetC + vertexSize * indexBuffer->getIndex(i);

					draw3DLine(*PositionA, *PositionB, *Color);
				}
				else
					draw3DLine(*PositionA, *PositionB);
			}
		}
		return;
	case scene::EPT_TRIANGLE_FAN:
		{
			// TODO: don't convert fan to list
			NewBuffer.reallocate(primitiveCount*3);

			for(u32 t = 0; t < primitiveCount; ++t)
			{
				NewBuffer.addIndex(indexBuffer->getIndex(0));
				NewBuffer.addIndex(indexBuffer->getIndex(t+1));
				NewBuffer.addIndex(indexBuffer->getIndex(t+2));
			}

			IndexPointer = &NewBuffer;
		}
		break;
	case scene::EPT_TRIANGLES:
		IndexPointer = indexBuffer;
		break;
	default:
		return;
	}

	// Supported are only built-in Irrlicht vertex formats.
	switch (vertexSize)
	{
	case sizeof(S3DVertex):
		drawClippedIndexedTriangleListT((S3DVertex*)vertexBuffer->getVertices(), vertexBuffer->getVertexCount(), (u16*)IndexPointer->getIndices(), primitiveCount);
		break;
	case sizeof(S3DVertex2TCoords):
		drawClippedIndexedTriangleListT((S3DVertex2TCoords*)vertexBuffer->getVertices(), vertexBuffer->getVertexCount(), (u16*)IndexPointer->getIndices(), primitiveCount);
		break;
	case sizeof(S3DVertexTangents):
		drawClippedIndexedTriangleListT((S3DVertexTangents*)vertexBuffer->getVertices(), vertexBuffer->getVertexCount(), (u16*)IndexPointer->getIndices(), primitiveCount);
		break;
	}
}


template<class VERTEXTYPE>
void CSoftwareDriver::drawClippedIndexedTriangleListT(const VERTEXTYPE* vertices,
	s32 vertexCount, const u16* indexList, s32 triangleCount)
{
	if (!RenderTargetSurface || !ZBuffer || !triangleCount)
		return;

	if (!checkPrimitiveCount(triangleCount))
		return;

	// arrays for storing clipped vertices
	core::array<VERTEXTYPE> clippedVertices;
	core::array<u16> clippedIndices;

	// calculate inverse world transformation
	core::matrix4 worldinv(TransformationMatrix[ETS_WORLD]);
	worldinv.makeInverse();

	// calculate view frustum planes
	scene::SViewFrustum frustum(TransformationMatrix[ETS_PROJECTION] * TransformationMatrix[ETS_VIEW]);

	// copy and transform clipping planes ignoring far plane
	core::plane3df planes[5]; // ordered by near, left, right, bottom, top
	for (int p=0; p<5; ++p)
		worldinv.transformPlane(frustum.planes[p+1], planes[p]);

	core::EIntersectionRelation3D inout[3]; // is point in front or back of plane?

	// temporary buffer for vertices to be clipped by all planes
	core::array<VERTEXTYPE> tClpBuf;
	int t;

	int i;
	for (i=0; i<triangleCount; ++i) // for all input triangles
	{
		// add next triangle to tempClipBuffer
		for (t=0; t<3; ++t)
			tClpBuf.push_back(vertices[indexList[(i*3)+t]]);

		for (int p=0; p<5; ++p) // for all clip planes
		for (int v=0; v<(int)tClpBuf.size(); v+=3) // for all vertices in temp clip buffer
		{
			int inside = 0;
			int outside = 0;

			// test intersection relation of the current vertices
			for (t=0; t<3; ++t)
			{
				inout[t] = planes[p].classifyPointRelation(tClpBuf[v+t].Pos);
				if (inout[t] != core::ISREL3D_FRONT)
					++inside;
				else
				if (inout[t] == core::ISREL3D_FRONT)
					++outside;
			}

			if (!outside)
			{
				// add all vertices to new buffer, this triangle needs no clipping.
				// so simply don't change this part of the temporary triangle buffer
				continue;
			}

			if (!inside)
			{
				// all vertices are outside, don't add this triangle, so erase this
				// triangle from the tClpBuf
				tClpBuf.erase(v,3);
				v -= 3;
				continue;
			}

			// this vertex has to be clipped by this clipping plane.

			// The following lines represent my try to implement some real clipping.
			// There is a bug somewhere, and after some time I've given up.
			// So now it is commented out, resulting that triangles which would need clipping
			// are simply taken out (in the next two lines).
#ifndef __SOFTWARE_CLIPPING_PROBLEM__
			tClpBuf.erase(v,3);
			v -= 3;
#endif

			/*
			// my idea is the following:
			// current vertex to next vertex relation:
			// out - out : add nothing
			// out -  in : add middle point
			// in -  out : add first and middle point
			// in -   in : add both


			// now based on the number of intersections, create new vertices
			// into tClpBuf (at the front for not letting them be clipped again)

			int added = 0;
			int prev = v+2;
			for (int index=v; index<v+3; ++index)
			{
				if (inout[prev] == core::ISREL3D_BACK)
				{
					if (inout[index] != core::ISREL3D_BACK)
					{
						VERTEXTYPE& vt1 = tClpBuf[prev];
						VERTEXTYPE& vt2 = tClpBuf[index];

						f32 fact = planes[p].getKnownIntersectionWithLine(vt1.Pos, vt2.Pos);
						VERTEXTYPE nvt;
						nvt.Pos = vt1.Pos.getInterpolated(vt2.Pos, fact);
						nvt.Color = vt1.Color.getInterpolated(vt2.Color, fact);
						nvt.TCoords = vt1.TCoords.getInterpolated(vt2.TCoords, fact);

						tClpBuf.push_front(nvt); ++index; ++prev; ++v;
						++added;
					}
				}
				else
				{
					if (inout[index] != core::ISREL3D_BACK)
					{
						VERTEXTYPE vt1 = tClpBuf[index];
						VERTEXTYPE vt2 = tClpBuf[prev];
						tClpBuf.push_front(vt1); ++index; ++prev; ++v;
						tClpBuf.push_front(vt2); ++index; ++prev; ++v;
						added+= 2;
					}
					else
					{
						// same as above, but other way round.
						VERTEXTYPE vt1 = tClpBuf[index];
						VERTEXTYPE vt2 = tClpBuf[prev];

						f32 fact = planes[p].getKnownIntersectionWithLine(vt1.Pos, vt2.Pos);
						VERTEXTYPE nvt;
						nvt.Pos = vt1.Pos.getInterpolated(vt2.Pos, fact);
						nvt.Color = vt1.Color.getInterpolated(vt2.Color, fact);
						nvt.TCoords = vt1.TCoords.getInterpolated(vt2.TCoords, fact);

						tClpBuf.push_front(vt2); ++index; ++prev; ++v;
						tClpBuf.push_front(nvt); ++index; ++prev; ++v;
						added += 2;
					}
				}

				prev = index;
			}

			// erase original vertices
			tClpBuf.erase(v,3);
			v -= 3;
			*/


		} // end for all clip planes

		// now add all remaining triangles in tempClipBuffer to clippedIndices
		// and clippedVertices array.
		if (clippedIndices.size() + tClpBuf.size() < 65535)
		for (t=0; t<(int)tClpBuf.size(); ++t)
		{
			clippedIndices.push_back(clippedVertices.size());
			clippedVertices.push_back(tClpBuf[t]);
		}
		tClpBuf.clear();

	} // end for all input triangles


	// draw newly created triangles.

	// -----------------------------------------------------------
	// here all triangles are being drawn. I put this in a separate
	// method, but the visual studio 6 compiler has great problems
	// with templates and didn't accept two template methods in this
	// class.

	// draw triangles

	scene::CVertexBuffer<S3DVertex>* vtxBuffer = new scene::CVertexBuffer<S3DVertex>();
	vtxBuffer->reallocate(clippedVertices.size());

	for (u32 i = 0; i < clippedVertices.size(); ++i)
		vtxBuffer->addVertex(&clippedVertices[i]);

	scene::CIndexBuffer* idxBuffer = new scene::CIndexBuffer(video::EIT_16BIT);
	idxBuffer->reallocate(clippedIndices.size());

	for (u32 i = 0; i < clippedIndices.size(); ++i)
		idxBuffer->addIndex(clippedIndices[i]);

	CNullDriver::drawVertexPrimitiveList(vtxBuffer, idxBuffer, VertexDescriptor[0], clippedIndices.size()/3, scene::EPT_TRIANGLES);

	if (TransformedPoints.size() < clippedVertices.size())
		TransformedPoints.set_used(clippedVertices.size());

	if (TransformedPoints.empty())
	{
		vtxBuffer->drop();
		idxBuffer->drop();

		return;
	}

	const VERTEXTYPE* currentVertex = clippedVertices.pointer();
	S2DVertex* tp = &TransformedPoints[0];

	core::dimension2d<u32> textureSize(0,0);
	f32 zDiv;

	if (Texture)
		textureSize = ((CSoftwareTexture*)Texture)->getTexture()->getDimension();

	f32 transformedPos[4]; // transform all points in the list

	core::matrix4 matrix(TransformationMatrix[ETS_PROJECTION]);
	matrix *= TransformationMatrix[ETS_VIEW];
	matrix *= TransformationMatrix[ETS_WORLD];

	s32 ViewTransformWidth = (ViewPortSize.Width>>1);
	s32 ViewTransformHeight = (ViewPortSize.Height>>1);

	for (i=0; i<(int)clippedVertices.size(); ++i)
	{
		transformedPos[0] = currentVertex->Pos.X;
		transformedPos[1] = currentVertex->Pos.Y;
		transformedPos[2] = currentVertex->Pos.Z;
		transformedPos[3] = 1.0f;

		matrix.multiplyWith1x4Matrix(transformedPos);
		zDiv = transformedPos[3] == 0.0f ? 1.0f : (1.0f / transformedPos[3]);

		tp->Pos.X = (s32)(ViewTransformWidth * (transformedPos[0] * zDiv) + (Render2DTranslation.X));
		tp->Pos.Y = (Render2DTranslation.Y - (s32)(ViewTransformHeight * (transformedPos[1] * zDiv)));
		tp->Color = currentVertex->Color.toA1R5G5B5();
		tp->ZValue = (TZBufferType)(32767.0f * zDiv);

		tp->TCoords.X = (s32)(currentVertex->TCoords.X * textureSize.Width);
		tp->TCoords.X <<= 8;
		tp->TCoords.Y = (s32)(currentVertex->TCoords.Y * textureSize.Height);
		tp->TCoords.Y <<= 8;

		++currentVertex;
		++tp;
	}

	// draw all transformed points from the index list
	CurrentTriangleRenderer->drawIndexedTriangleList(&TransformedPoints[0],
		clippedVertices.size(), clippedIndices.pointer(), clippedIndices.size()/3);

	vtxBuffer->drop();
	idxBuffer->drop();
}


//! Draws a 3d line.
void CSoftwareDriver::draw3DLine(const core::vector3df& start,
				const core::vector3df& end, SColor color)
{
	core::vector3df vect = start.crossProduct(end);
	vect.normalize();
	vect *= Material.Thickness*0.3f;

	S3DVertex vtx[4];

	vtx[0].Color = color;
	vtx[1].Color = color;
	vtx[2].Color = color;
	vtx[3].Color = color;

	vtx[0].Pos = start;
	vtx[1].Pos = end;

	vtx[2].Pos = start + vect;
	vtx[3].Pos = end + vect;

	u16 idx[12] = {0,1,2, 0,2,1, 0,1,3, 0,3,1};

	scene::CVertexBuffer<S3DVertex>* vtxBuffer = new scene::CVertexBuffer<S3DVertex>();
	vtxBuffer->reallocate(4);

	for (u32 i = 0; i < 4; ++i)
		vtxBuffer->addVertex(&vtx[i]);

	scene::CIndexBuffer* idxBuffer = new scene::CIndexBuffer(video::EIT_16BIT);
	idxBuffer->reallocate(12);

	for (u32 i = 0; i < 12; ++i)
		idxBuffer->addIndex(idx[i]);

	drawIndexedTriangleList((scene::IVertexBuffer*)vtxBuffer, idxBuffer, VertexDescriptor[0], 4);

	vtxBuffer->drop();
	idxBuffer->drop();
}


//! clips a triangle against the viewing frustum
void CSoftwareDriver::clipTriangle(f32* transformedPos)
{
}


//! Only used by the internal engine. Used to notify the driver that
//! the window was resized.
void CSoftwareDriver::OnResize(const core::dimension2d<u32>& size)
{
	// make sure width and height are multiples of 2
	core::dimension2d<u32> realSize(size);

	if (realSize.Width % 2)
		realSize.Width += 1;

	if (realSize.Height % 2)
		realSize.Height += 1;

	if (ScreenSize != realSize)
	{
		if (ViewPort.getWidth() == (s32)ScreenSize.Width &&
			ViewPort.getHeight() == (s32)ScreenSize.Height)
		{
			ViewPort = core::rect<s32>(core::position2d<s32>(0,0),
										core::dimension2di(realSize));
		}

		ScreenSize = realSize;

		bool resetRT = (RenderTargetSurface == BackBuffer);

		if (BackBuffer)
			BackBuffer->drop();
		BackBuffer = new CImage(ECF_A1R5G5B5, realSize);

		if (resetRT)
			setRenderTarget(BackBuffer);
	}
}

//! returns the current render target size
const core::dimension2d<u32>& CSoftwareDriver::getCurrentRenderTargetSize() const
{
	return RenderTargetSize;
}


//! draws an 2d image, using a color (if color is other then Color(255,255,255,255)) and the alpha channel of the texture if wanted.
void CSoftwareDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos,
					const core::rect<s32>& sourceRect,
					const core::rect<s32>* clipRect, SColor color,
					bool useAlphaChannelOfTexture)
{
	if (texture)
	{
		if (texture->getDriverType() != EDT_SOFTWARE)
		{
			os::Printer::log("Fatal Error: Tried to copy from a surface not owned by this driver.", ELL_ERROR);
			return;
		}

		if (useAlphaChannelOfTexture)
			((CSoftwareTexture*)texture)->getImage()->copyToWithAlpha(
				RenderTargetSurface, destPos, sourceRect, color, clipRect);
		else
			((CSoftwareTexture*)texture)->getImage()->copyTo(
				RenderTargetSurface, destPos, sourceRect, clipRect);
	}
}



//! Draws a 2d line.
void CSoftwareDriver::draw2DLine(const core::position2d<s32>& start,
				const core::position2d<s32>& end,
				SColor color)
{
	drawLine(RenderTargetSurface, start, end, color );
}


//! Draws a pixel
void CSoftwareDriver::drawPixel(u32 x, u32 y, const SColor & color)
{
	BackBuffer->setPixel(x, y, color, true);
}


//! draw a 2d rectangle
void CSoftwareDriver::draw2DRectangle(SColor color, const core::rect<s32>& pos,
					const core::rect<s32>* clip)
{
	if (clip)
	{
		core::rect<s32> p(pos);

		p.clipAgainst(*clip);

		if(!p.isValid())
			return;

		drawRectangle(RenderTargetSurface, p, color);
	}
	else
	{
		if(!pos.isValid())
			return;

		drawRectangle(RenderTargetSurface, pos, color);
	}
}


//!Draws an 2d rectangle with a gradient.
void CSoftwareDriver::draw2DRectangle(const core::rect<s32>& pos,
	SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown,
	const core::rect<s32>* clip)
{
	// TODO: implement
	draw2DRectangle(colorLeftUp, pos, clip);
}


//! \return Returns the name of the video driver. Example: In case of the Direct3D8
//! driver, it would return "Direct3D8.1".
const wchar_t* CSoftwareDriver::getName() const
{
	return L"Irrlicht Software Driver 1.0";
}


//! Returns type of video driver
E_DRIVER_TYPE CSoftwareDriver::getDriverType() const
{
	return EDT_SOFTWARE;
}


//! returns color format
ECOLOR_FORMAT CSoftwareDriver::getColorFormat() const
{
	if (BackBuffer)
		return BackBuffer->getColorFormat();
	else
		return CNullDriver::getColorFormat();
}


//! Returns the transformation set by setTransform
const core::matrix4& CSoftwareDriver::getTransform(E_TRANSFORMATION_STATE state) const
{
	return TransformationMatrix[state];
}


//! Creates a render target texture.
ITexture* CSoftwareDriver::addRenderTargetTexture(const core::dimension2d<u32>& size,
												  const io::path& name,
												  const ECOLOR_FORMAT format)
{
	IImage* img = createImage(video::ECF_A1R5G5B5, size);
	ITexture* tex = new CSoftwareTexture(img, name, true);
	img->drop();
	addTexture(tex);
	tex->drop();
	return tex;
}


//! Clears the ZBuffer.
void CSoftwareDriver::clearZBuffer()
{
	if (ZBuffer)
		ZBuffer->clear();
}


//! Returns an image created from the last rendered frame.
IImage* CSoftwareDriver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
{
	if (target != video::ERT_FRAME_BUFFER)
		return 0;

	if (BackBuffer)
	{
		IImage* tmp = createImage(BackBuffer->getColorFormat(), BackBuffer->getDimension());
		BackBuffer->copyTo(tmp);
		return tmp;
	}
	else
		return 0;
}


//! Returns the maximum amount of primitives (mostly vertices) which
//! the device is able to render with one drawIndexedTriangleList
//! call.
u32 CSoftwareDriver::getMaximalPrimitiveCount() const
{
	return 0x00800000;
}

} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_SOFTWARE_

namespace irr
{
namespace video
{


//! creates a video driver
IVideoDriver* createSoftwareDriver(const core::dimension2d<u32>& windowSize, bool fullscreen, io::IFileSystem* io, video::IImagePresenter* presenter)
{
	#ifdef _IRR_COMPILE_WITH_SOFTWARE_
	return new CSoftwareDriver(windowSize, fullscreen, io, presenter);
	#else
	return 0;
	#endif
}


} // end namespace video
} // end namespace irr

