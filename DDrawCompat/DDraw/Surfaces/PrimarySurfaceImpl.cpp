#include "Common/CompatPtr.h"
#include "D3dDdi/KernelModeThunks.h"
#include "DDraw/DirectDrawClipper.h"
#include "DDraw/DirectDrawPalette.h"
#include "DDraw/DirectDrawSurface.h"
#include "DDraw/RealPrimarySurface.h"
#include "DDraw/Surfaces/PrimarySurface.h"
#include "DDraw/Surfaces/PrimarySurfaceImpl.h"
#include "Dll/Procs.h"
#include "Gdi/Gdi.h"
#include "Gdi/Region.h"
#include "Gdi/VirtualScreen.h"

namespace
{
	template <typename TSurface>
	void bltToGdi(TSurface* This, LPRECT lpDestRect, TSurface* lpDDSrcSurface, LPRECT lpSrcRect,
		DWORD dwFlags, LPDDBLTFX lpDDBltFx)
	{
		if (!lpDestRect)
		{
			return;
		}

		CompatPtr<IDirectDrawClipper> clipper;
		CompatVtable<Vtable<TSurface>>::s_origVtable.GetClipper(This, &clipper.getRef());
		if (!clipper)
		{
			return;
		}

		Gdi::Region clipRgn(DDraw::DirectDrawClipper::getClipRgn(*clipper));
		RECT monitorRect = D3dDdi::KernelModeThunks::getMonitorRect();
		RECT virtualScreenBounds = Gdi::VirtualScreen::getBounds();
		clipRgn.offset(monitorRect.left, monitorRect.top);
		clipRgn &= virtualScreenBounds;
		clipRgn -= monitorRect;
		if (clipRgn.isEmpty())
		{
			return;
		}

		auto gdiSurface(Gdi::VirtualScreen::createSurface(virtualScreenBounds));
		if (!gdiSurface)
		{
			return;
		}

		CompatPtr<IDirectDrawClipper> gdiClipper;
		CALL_ORIG_PROC(DirectDrawCreateClipper, 0, &gdiClipper.getRef(), nullptr);
		if (!gdiClipper)
		{
			return;
		}

		RECT dstRect = *lpDestRect;
		OffsetRect(&dstRect, monitorRect.left - virtualScreenBounds.left, monitorRect.top - virtualScreenBounds.top);
		clipRgn.offset(-virtualScreenBounds.left, -virtualScreenBounds.top);
		DDraw::DirectDrawClipper::setClipRgn(*gdiClipper, clipRgn);

		auto srcSurface(CompatPtr<IDirectDrawSurface7>::from(lpDDSrcSurface));
		gdiSurface->SetClipper(gdiSurface, gdiClipper);
		gdiSurface->Blt(gdiSurface, &dstRect, srcSurface, lpSrcRect, dwFlags, lpDDBltFx);
		gdiSurface->SetClipper(gdiSurface, nullptr);
	}

	void restorePrimaryCaps(DWORD& caps)
	{
		caps &= ~DDSCAPS_OFFSCREENPLAIN;
		caps |= DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE;

		if ((caps & DDSCAPS_SYSTEMMEMORY) &&
			!(DDraw::PrimarySurface::getOrigCaps() & DDSCAPS_SYSTEMMEMORY))
		{
			caps &= ~DDSCAPS_SYSTEMMEMORY;
			caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
		}
	}
}

namespace DDraw
{
	template <typename TSurface>
	PrimarySurfaceImpl<TSurface>::PrimarySurfaceImpl(SurfaceImpl& impl) : m_impl(impl)
	{
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Blt(
		TSurface* This, LPRECT lpDestRect, TSurface* lpDDSrcSurface, LPRECT lpSrcRect,
		DWORD dwFlags, LPDDBLTFX lpDDBltFx)
	{
		if (RealPrimarySurface::isLost())
		{
			return DDERR_SURFACELOST;
		}

		HRESULT result = m_impl.Blt(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
		if (SUCCEEDED(result))
		{
			bltToGdi(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
			RealPrimarySurface::update();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::BltFast(
		TSurface* This, DWORD dwX, DWORD dwY, TSurface* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans)
	{
		if (RealPrimarySurface::isLost())
		{
			return DDERR_SURFACELOST;
		}

		HRESULT result = m_impl.BltFast(This, dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans);
		if (SUCCEEDED(result))
		{
			RealPrimarySurface::update();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Flip(TSurface* This, TSurface* lpDDSurfaceTargetOverride, DWORD dwFlags)
	{
		const bool wait = (dwFlags & DDFLIP_WAIT) || !(dwFlags & DDFLIP_DONOTWAIT) &&
			CompatVtable<IDirectDrawSurface7Vtbl>::s_origVtablePtr == static_cast<void*>(This->lpVtbl);
		if (!DDraw::RealPrimarySurface::waitForFlip(Surface::getSurface(*This), wait))
		{
			return DDERR_WASSTILLDRAWING;
		}

		return RealPrimarySurface::flip(CompatPtr<IDirectDrawSurface7>::from(lpDDSurfaceTargetOverride), dwFlags);
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetCaps(TSurface* This, TDdsCaps* lpDDSCaps)
	{
		HRESULT result = m_impl.GetCaps(This, lpDDSCaps);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSCaps->dwCaps);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetSurfaceDesc(TSurface* This, TSurfaceDesc* lpDDSurfaceDesc)
	{
		HRESULT result = m_impl.GetSurfaceDesc(This, lpDDSurfaceDesc);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSurfaceDesc->ddsCaps.dwCaps);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::IsLost(TSurface* This)
	{
		HRESULT result = m_impl.IsLost(This);
		if (SUCCEEDED(result))
		{
			result = RealPrimarySurface::isLost() ? DDERR_SURFACELOST : DD_OK;
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Lock(
		TSurface* This, LPRECT lpDestRect, TSurfaceDesc* lpDDSurfaceDesc,
		DWORD dwFlags, HANDLE hEvent)
	{
		if (RealPrimarySurface::isLost())
		{
			return DDERR_SURFACELOST;
		}

		HRESULT result = m_impl.Lock(This, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSurfaceDesc->ddsCaps.dwCaps);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::ReleaseDC(TSurface* This, HDC hDC)
	{
		HRESULT result = m_impl.ReleaseDC(This, hDC);
		if (SUCCEEDED(result))
		{
			RealPrimarySurface::update();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Restore(TSurface* This)
	{
		HRESULT result = IsLost(This);
		if (FAILED(result))
		{
			result = RealPrimarySurface::restore();
			if (SUCCEEDED(result))
			{
				result = m_impl.Restore(This);
				if (SUCCEEDED(result))
				{
					PrimarySurface::onRestore();
				}
			}
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::SetPalette(TSurface* This, LPDIRECTDRAWPALETTE lpDDPalette)
	{
		if (lpDDPalette)
		{
			DirectDrawPalette::waitForNextUpdate();
		}
		if (lpDDPalette == PrimarySurface::s_palette)
		{
			return DD_OK;
		}

		HRESULT result = m_impl.SetPalette(This, lpDDPalette);
		if (SUCCEEDED(result))
		{
			PrimarySurface::s_palette = lpDDPalette;
			PrimarySurface::updatePalette();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Unlock(TSurface* This, TUnlockParam lpRect)
	{
		HRESULT result = m_impl.Unlock(This, lpRect);
		if (SUCCEEDED(result))
		{
			RealPrimarySurface::update();
		}
		return result;
	}

	template PrimarySurfaceImpl<IDirectDrawSurface>;
	template PrimarySurfaceImpl<IDirectDrawSurface2>;
	template PrimarySurfaceImpl<IDirectDrawSurface3>;
	template PrimarySurfaceImpl<IDirectDrawSurface4>;
	template PrimarySurfaceImpl<IDirectDrawSurface7>;
}
