// ExosMoteur.cpp : Ce fichier contient la fonction 'main'. L'exécution du programme commence et se termine à cet endroit.
//
#include <windows.h>
#include <WindowsX.h>
#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <d3d12.h>
#include <dxgi1_4.h>
#include <iostream>
#include <cassert>
#include <wrl.h>
#include <stdexcept>

#include "GameTimer.h"
#include "d3dx12.h"

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")



bool mAppPaused = false;
bool mMinimized = false;
bool mMaximized = false;  // is the application maximized?
bool mResizing = false;   // are the resize bars being dragged?
HINSTANCE mhAppInst = nullptr;
bool mFullscreenState = false;
HWND mhMainWnd = nullptr;
bool m4xMsaaState = false;
UINT m4xMsaaQuality = 0;
IDXGIFactory4* mdxgiFactory;
IDXGISwapChain* mSwapChain;
ID3D12Device* md3dDevice;
ID3D12Fence* mFence;
UINT64 mCurrentFence = 0;
UINT mRtvDescriptorSize = 0;
UINT mDsvDescriptorSize = 0;
UINT mCbvSrvUavDescriptorSize = 0;
DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
ID3D12CommandQueue* mCommandQueue;
ID3D12CommandAllocator* mDirectCmdListAlloc;
ID3D12GraphicsCommandList* mCommandList;

ID3D12DescriptorHeap* mRtvHeap;
ID3D12DescriptorHeap* mDsvHeap;

static const int SwapChainBufferCount = 2;
int mCurrBackBuffer = 0;
ID3D12Resource* mSwapChainBuffer[SwapChainBufferCount];
ID3D12Resource* mDepthStencilBuffer;
int mClientWidth = 800;
int mClientHeight = 600;

GameTimer mTimer;
D3D_DRIVER_TYPE md3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

D3D12_VIEWPORT mScreenViewport;
D3D12_RECT mScissorRect;

void OnMouseDown(WPARAM btnState, int x, int y) { }
void OnMouseUp(WPARAM btnState, int x, int y) { }
void OnMouseMove(WPARAM btnState, int x, int y) { }

D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView()
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView()
{
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence, mCurrentFence);

	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, L"false", false, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		mFence->SetEventOnCompletion(mCurrentFence, eventHandle);

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void CreateSwapChain()
{
	// Release the previous swapchain we will be recreating.
	mSwapChain=nullptr;

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = mhMainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// Note: Swap chain uses queue to perform flush.
	mdxgiFactory->CreateSwapChain(
		mCommandQueue,
		&sd,
		&mSwapChain);
}


void OnResize()
{
	assert(md3dDevice);
	assert(mSwapChain);
	assert(mDirectCmdListAlloc);

	// Flush before changing any resources.
	FlushCommandQueue();

	mCommandList->Reset(mDirectCmdListAlloc, nullptr);

	// Release the previous resources we will be recreating.
	for (int i = 0; i < SwapChainBufferCount; ++i)
		mSwapChainBuffer[i] = nullptr;
	mDepthStencilBuffer = nullptr;

	// Resize the swap chain.
	mSwapChain->ResizeBuffers(
		SwapChainBufferCount,
		mClientWidth, mClientHeight,
		mBackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

	mCurrBackBuffer = 0;
	//create render target view.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < SwapChainBufferCount; i++)
	{
		mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i]));
		md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i], nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}

	// Create the depth/stencil buffer and view.
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = mClientWidth;
	depthStencilDesc.Height = mClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;

	// Correction 11/12/2016: SSAO chapter requires an SRV to the depth buffer to read from 
	// the depth buffer.  Therefore, because we need to create two views to the same resource:
	//   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
	//   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
	// we need to create the depth buffer resource with a typeless format.  
	depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;

	depthStencilDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	CD3DX12_HEAP_PROPERTIES prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	md3dDevice->CreateCommittedResource(
		&prop,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(&mDepthStencilBuffer));

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = mDepthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	md3dDevice->CreateDepthStencilView(mDepthStencilBuffer, &dsvDesc, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

	CD3DX12_RESOURCE_BARRIER bar  = CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer,D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

	// Transition the resource from its initial state to be used as a depth buffer.
	mCommandList->ResourceBarrier(1, &bar);

	// Execute the resize commands.
	mCommandList->Close();
	ID3D12CommandList* cmdsLists[] = { mCommandList };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until resize is complete.
	FlushCommandQueue();

	// Update the viewport transform to cover the client area.
	mScreenViewport.TopLeftX = 0;
	mScreenViewport.TopLeftY = 0;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MinDepth = 0.0f;
	mScreenViewport.MaxDepth = 1.0f;

	mScissorRect = { 0, 0, mClientWidth, mClientHeight };
}

void Set4xMsaaState(bool value)
{
	if (m4xMsaaState != value)
	{
		m4xMsaaState = value;

		// Recreate the swapchain and buffers with new multisample settings.
		CreateSwapChain();
		OnResize();
	}
}
LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		// WM_ACTIVATE is sent when the window is activated or deactivated.  
		// We pause the game when the window is deactivated and unpause it 
		// when it becomes active.  
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			mAppPaused = true;
			mTimer.Stop();
		}
		else
		{
			mAppPaused = false;
			mTimer.Start();
		}
		return 0;

		// WM_SIZE is sent when the user resizes the window.  
	case WM_SIZE:
		// Save the new client area dimensions.
		mClientWidth = LOWORD(lParam);
		mClientHeight = HIWORD(lParam);
		if (md3dDevice)
		{
			if (wParam == SIZE_MINIMIZED)
			{
				mAppPaused = true;
				mMinimized = true;
				mMaximized = false;
			}
			else if (wParam == SIZE_MAXIMIZED)
			{
				mAppPaused = false;
				mMinimized = false;
				mMaximized = true;
				OnResize();
			}
			else if (wParam == SIZE_RESTORED)
			{

				// Restoring from minimized state?
				if (mMinimized)
				{
					mAppPaused = false;
					mMinimized = false;
					OnResize();
				}

				// Restoring from maximized state?
				else if (mMaximized)
				{
					mAppPaused = false;
					mMaximized = false;
					OnResize();
				}
				else if (mResizing)
				{
					// If user is dragging the resize bars, we do not resize 
					// the buffers here because as the user continuously 
					// drags the resize bars, a stream of WM_SIZE messages are
					// sent to the window, and it would be pointless (and slow)
					// to resize for each WM_SIZE message received from dragging
					// the resize bars.  So instead, we reset after the user is 
					// done resizing the window and releases the resize bars, which 
					// sends a WM_EXITSIZEMOVE message.
				}
				else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
				{
					OnResize();
				}
			}
		}
		return 0;

		// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
	case WM_ENTERSIZEMOVE:
		mAppPaused = true;
		mResizing = true;
		mTimer.Stop();
		return 0;

		// WM_EXITSIZEMOVE is sent when the user releases the resize bars.
		// Here we reset everything based on the new window dimensions.
	case WM_EXITSIZEMOVE:
		mAppPaused = false;
		mResizing = false;
		mTimer.Start();
		OnResize();
		return 0;

		// WM_DESTROY is sent when the window is being destroyed.
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

		// The WM_MENUCHAR message is sent when a menu is active and the user presses 
		// a key that does not correspond to any mnemonic or accelerator key. 
	case WM_MENUCHAR:
		// Don't beep when we alt-enter.
		return MAKELRESULT(0, MNC_CLOSE);

		// Catch this message so to prevent the window from becoming too small.
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
		((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
		return 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_MOUSEMOVE:
		OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_KEYUP:
		if (wParam == VK_ESCAPE)
		{
			PostQuitMessage(0);
		}
		else if ((int)wParam == VK_F2)
			Set4xMsaaState(!m4xMsaaState);

		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}






const float AliceBlue[4] = { 0.941176534f, 0.972549081f, 1.f, 1.f  };
const float AntiqueWhite[4] =  { 0.980392218f, 0.921568692f, 0.843137324f, 1.f  };
const float Aqua[4] = { 0.f, 1.f, 1.f, 1.f } ;
const float Aquamarine[4] =  { 0.498039246f, 1.f, 0.831372619f, 1.f  };
const float Azure[4] = {  0.941176534f, 1.f, 1.f, 1.f } ;
const float Beige[4] =  { 0.960784376f, 0.960784376f, 0.862745166f, 1.f  };
const float Bisque[4] =  { 1.f, 0.894117713f, 0.768627524f, 1.f } ;
const float Black[4] = {  0.f, 0.f, 0.f, 1.f } ;
const float BlanchedAlmond[4] =  { 1.f, 0.921568692f, 0.803921640f, 1.f  };
const float Blue[4] = { 0.f, 0.f, 1.f, 1.f } ;
const float BlueViolet[4] =  { 0.541176498f, 0.168627456f, 0.886274576f, 1.f  };
const float Brown[4] = {  0.647058845f, 0.164705887f, 0.164705887f, 1.f } ;
const float BurlyWood[4] =  { 0.870588303f, 0.721568644f, 0.529411793f, 1.f  };
const float CadetBlue[4] =  { 0.372549027f, 0.619607866f, 0.627451003f, 1.f  };
const float Chartreuse[4] =  { 0.498039246f, 1.f, 0.f, 1.f };
const float Chocolate[4] = { 0.823529482f, 0.411764741f, 0.117647067f, 1.f  };
const float Coral[4] = { 1.f, 0.498039246f, 0.313725501f, 1.f } ;
const float CornflowerBlue[4] =  { 0.392156899f, 0.584313750f, 0.929411829f, 1.f  };
const float Cornsilk[4] = {  1.f, 0.972549081f, 0.862745166f, 1.f } ;
const float Crimson[4] =  { 0.862745166f, 0.078431375f, 0.235294133f, 1.f  };
const float Cyan[4] =  { 0.f, 1.f, 1.f, 1.f } ;
const float DarkBlue[4] =  { 0.f, 0.f, 0.545098066f, 1.f  };
const float DarkCyan[4] =  { 0.f, 0.545098066f, 0.545098066f, 1.f } ;
const float DarkGoldenrod[4] =  { 0.721568644f, 0.525490224f, 0.043137256f, 1.f  };
const float DarkGray[4] = {  0.662745118f, 0.662745118f, 0.662745118f, 1.f  };
const float DarkGreen[4] =  { 0.f, 0.392156899f, 0.f, 1.f } ;
const float DarkKhaki[4] =  { 0.741176486f, 0.717647076f, 0.419607878f, 1.f  };
const float DarkMagenta[4] =  { 0.545098066f, 0.f, 0.545098066f, 1.f } ;
const float DarkOliveGreen[4] =  { 0.333333343f, 0.419607878f, 0.184313729f, 1.f };
const float DarkOrange[4] = { 1.f, 0.549019635f, 0.f, 1.f } ;
const float DarkOrchid[4] =  { 0.600000024f, 0.196078449f, 0.800000072f, 1.f  };
const float DarkRed[4] = {  0.545098066f, 0.f, 0.f, 1.f } ;
const float DarkSalmon[4] = {  0.913725555f, 0.588235319f, 0.478431404f, 1.f  };
const float DarkSeaGreen[4] =  { 0.560784340f, 0.737254918f, 0.545098066f, 1.f  };
const float DarkSlateBlue[4] =  { 0.282352954f, 0.239215702f, 0.545098066f, 1.f  };
const float DarkSlateGray[4] =  { 0.184313729f, 0.309803933f, 0.309803933f, 1.f  };
const float DarkTurquoise[4] = { 0.f, 0.807843208f, 0.819607913f, 1.  };
const float DarkViolet[4] = {  0.580392182f, 0.f, 0.827451050f, 1.f  };
const float DeepPink[4] = {  1.f, 0.078431375f, 0.576470613f, 1.f  };
const float DeepSkyBlue[4] =  { 0.f, 0.749019623f, 1.f, 1.f } ;
const float DimGray[4] = {  0.411764741f, 0.411764741f, 0.411764741f, 1.f  };
const float DodgerBlue[4] =  { 0.117647067f, 0.564705908f, 1.f, 1.f } ;
const float Firebrick[4] =  { 0.698039234f, 0.133333340f, 0.133333340f, 1.f };
const float FloralWhite[4] ={  1.f, 0.980392218f, 0.941176534f, 1.f } ;
const float ForestGreen[4] = { 0.133333340f, 0.545098066f, 0.133333340f, 1.f  };
const float Fuchsia[4] = { 1.f, 0.f, 1.f, 1.f } ;
const float Gainsboro[4] = {  0.862745166f, 0.862745166f, 0.862745166f, 1.f  };
const float GhostWhite[4] =  { 0.972549081f, 0.972549081f, 1.f, 1.f  };
const float Gold[4] = { 1.f, 0.843137324f, 0.f, 1.f } ;
const float Goldenrod[4] =  { 0.854902029f, 0.647058845f, 0.125490203f, 1.f  };
const float Gray[4] =  { 0.501960814f, 0.501960814f, 0.501960814f, 1.f  };
const float Green[4] =  { 0.f, 0.501960814f, 0.f, 1.f } ;
const float GreenYellow[4] =   { 0.678431392f, 1.f, 0.184313729f, 1.f  };
const float Honeydew[4] =  { 0.941176534f, 1.f, 0.941176534f, 1.f  };
const float HotPink[4] =  { 1.f, 0.411764741f, 0.705882370f, 1.f } ;
const float IndianRed[4] =  { 0.803921640f, 0.360784322f, 0.360784322f, 1.f  };
const float Indigo[4] =  { 0.294117659f, 0.f, 0.509803951f, 1.f  };
const float Ivory[4] =  { 1.f, 1.f, 0.941176534f, 1.f } ;
const float Khaki[4] =  { 0.941176534f, 0.901960850f, 0.549019635f, 1.f  };
const float Lavender[4] =  { 0.901960850f, 0.901960850f, 0.980392218f, 1.f  };
const float LavenderBlush[4] =  { 1.f, 0.941176534f, 0.960784376f, 1.f };
const float LawnGreen[4] =  { 0.486274540f, 0.988235354f, 0.f, 1.f } ;
const float LemonChiffon[4] =  { 1.f, 0.980392218f, 0.803921640f, 1.f } ;
const float LightBlue[4] =  { 0.678431392f, 0.847058892f, 0.901960850f, 1.f  };
const float LightCoral[4] =  { 0.941176534f, 0.501960814f, 0.501960814f, 1.f  };
const float LightCyan[4] =  { 0.878431439f, 1.f, 1.f, 1.f } ;
const float LightGoldenrodYellow[4] = { 0.980392218f, 0.980392218f, 0.823529482f, 1.f  };
const float LightGray[4] = {  0.827451050f, 0.827451050f, 0.827451050f, 1.f  };
const float LightGreen[4] =  { 0.564705908f, 0.933333397f, 0.564705908f, 1.f  };
const float LightPink[4] =  { 1.f, 0.713725507f, 0.756862819f, 1.f  };
const float LightSalmon[4] =  { 1.f, 0.627451003f, 0.478431404f, 1.f  };
const float LightSeaGreen[4] =  { 0.125490203f, 0.698039234f, 0.666666687f, 1.f  };
const float LightSkyBlue[4] =  { 0.529411793f, 0.807843208f, 0.980392218f, 1.f  };
const float LightSlateGray[4] =  { 0.466666698f, 0.533333361f, 0.600000024f, 1.f  };
const float LightSteelBlue[4] =  { 0.690196097f, 0.768627524f, 0.870588303f, 1.f  };
const float LightYellow[4] = { 1.f, 1.f, 0.878431439f, 1.f  };
const float Lime[4] = {  0.f, 1.f, 0.f, 1.f } ;
const float LimeGreen[4] =  { 0.196078449f, 0.803921640f, 0.196078449f, 1.f  };
const float Linen[4] =  { 0.980392218f, 0.941176534f, 0.901960850f, 1.f  };
const float Magenta[4] =  { 1.f, 0.f, 1.f, 1.f } ;
const float Maroon[4] =  { 0.501960814f, 0.f, 0.f, 1.f } ;
const float MediumAquamarine[4] =  { 0.400000036f, 0.803921640f, 0.666666687f, 1.f  };
const float MediumBlue[4] = {  0.f, 0.f, 0.803921640f, 1.f } ;
const float MediumOrchid[4] =  { 0.729411781f, 0.333333343f, 0.827451050f, 1.f  };
const float MediumPurple[4] =  { 0.576470613f, 0.439215720f, 0.858823597f, 1.f  };
const float MediumSeaGreen[4] =  { 0.235294133f, 0.701960802f, 0.443137288f, 1.f };
const float MediumSlateBlue[4] =  { 0.482352972f, 0.407843173f, 0.933333397f, 1.f };
const float MediumSpringGreen[4] =  { 0.f, 0.980392218f, 0.603921592f, 1.f } ;
const float MediumTurquoise[4] =  { 0.282352954f, 0.819607913f, 0.800000072f, 1.f };
const float MediumVioletRed[4] =  { 0.780392230f, 0.082352944f, 0.521568656f, 1.f };
const float MidnightBlue[4] =  { 0.098039225f, 0.098039225f, 0.439215720f, 1.f  };
const float MintCream[4] =  { 0.960784376f, 1.f, 0.980392218f, 1.f  };
const float MistyRose[4] =   { 1.f, 0.894117713f, 0.882353008f, 1.f  };
const float Moccasin[4] =  { 1.f, 0.894117713f, 0.709803939f, 1.f  };
const float NavajoWhite[4] =  { 1.f, 0.870588303f, 0.678431392f, 1.f  };
const float Navy[4] = {  0.f, 0.f, 0.501960814f, 1.f } ;
const float OldLace[4] =  { 0.992156923f, 0.960784376f, 0.901960850f, 1.f  };
const float Olive[4] =  { 0.501960814f, 0.501960814f, 0.f, 1.f } ;
const float OliveDrab[4] =  { 0.419607878f, 0.556862772f, 0.137254909f, 1.f  };
const float Orange[4] =  { 1.f, 0.647058845f, 0.f, 1.f } ;
const float OrangeRed[4] =  { 1.f, 0.270588249f, 0.f, 1.f } ;
const float Orchid[4] =  { 0.854902029f, 0.439215720f, 0.839215755f, 1.f } ;
const float PaleGoldenrod[4] =  { 0.933333397f, 0.909803987f, 0.666666687f, 1.f  };
const float PaleGreen[4] =  { 0.596078455f, 0.984313786f, 0.596078455f, 1.f  };
const float PaleTurquoise[4] =  { 0.686274529f, 0.933333397f, 0.933333397f, 1.f  };
const float PaleVioletRed[4] =  { 0.858823597f, 0.439215720f, 0.576470613f, 1.f  };
const float PapayaWhip[4] =  { 1.f, 0.937254965f, 0.835294187f, 1.f };
const float PeachPuff[4] =  { 1.f, 0.854902029f, 0.725490212f, 1.f  };
const float Peru[4] = {  0.803921640f, 0.521568656f, 0.247058839f, 1.f  };
const float Pink[4] = {  1.f, 0.752941251f, 0.796078503f, 1.f } ;
const float Plum[4] = {  0.866666734f, 0.627451003f, 0.866666734f, 1.f } ;
const float PowderBlue[4] =  { 0.690196097f, 0.878431439f, 0.901960850f, 1.f  };
const float Purple[4] =  { 0.501960814f, 0.f, 0.501960814f, 1.f  };
const float Red[4] =  { 1.f, 0.f, 0.f, 1.f } ;
const float RosyBrown[4] = {  0.737254918f, 0.560784340f, 0.560784340f, 1.f  };
const float RoyalBlue[4] = {  0.254901975f, 0.411764741f, 0.882353008f, 1.f  };
const float SaddleBrown[4] =  { 0.545098066f, 0.270588249f, 0.074509807f, 1.f  };
const float Salmon[4] = {  0.980392218f, 0.501960814f, 0.447058856f, 1.f  };
const float SandyBrown[4] =  { 0.956862807f, 0.643137276f, 0.376470625f, 1.f  };
const float SeaGreen[4] =  { 0.180392161f, 0.545098066f, 0.341176480f, 1.f  };
const float SeaShell[4] =  { 1.f, 0.960784376f, 0.933333397f, 1.f } ;
const float Sienna[4] =  { 0.627451003f, 0.321568638f, 0.176470593f, 1.f  };
const float Silver[4] =  { 0.752941251f, 0.752941251f, 0.752941251f, 1.f  };
const float SkyBlue[4] =  { 0.529411793f, 0.807843208f, 0.921568692f, 1.f  };
const float SlateBlue[4] = {  0.415686309f, 0.352941185f, 0.803921640f, 1.f  };
const float SlateGray[4] = {  0.439215720f, 0.501960814f, 0.564705908f, 1.f } ;
const float Snow[4] =  { 1.f, 0.980392218f, 0.980392218f, 1.f  };
const float SpringGreen[4] =  { 0.f, 1.f, 0.498039246f, 1.f } ;
const float SteelBlue[4] =  { 0.274509817f, 0.509803951f, 0.705882370f, 1.f  };
const float Tan[4] = {  0.823529482f, 0.705882370f, 0.549019635f, 1.f  };
const float Teal[4] =  { 0.f, 0.501960814f, 0.501960814f, 1.f } ;
const float Thistle[4] =  { 0.847058892f, 0.749019623f, 0.847058892f, 1.f  };
const float Tomato[4] =  { 1.f, 0.388235331f, 0.278431386f, 1.f } ;
const float Transparent[4] =  { 0.f, 0.f, 0.f, 0.f } ;
const float Turquoise[4] = { 0.250980407f, 0.878431439f, 0.815686345f, 1.f  };
const float Violet[4] =  { 0.933333397f, 0.509803951f, 0.933333397f, 1.f };
const float Wheat[4] =  { 0.960784376f, 0.870588303f, 0.701960802f, 1.f };
const float White[4] =  { 1.f, 1.f, 1.f, 1.f } ;
const float WhiteSmoke[4] =  { 0.960784376f, 0.960784376f, 0.960784376f, 1.f } ;
const float Yellow[4] = {  1.f, 1.f, 0.f, 1.f } ;
const float YellowGreen[4] =  { 0.603921592f, 0.803921640f, 0.196078449f, 1.f } ;








































void Draw(const GameTimer& gt)
{
	CD3DX12_RESOURCE_BARRIER bar = CD3DX12_RESOURCE_BARRIER::Transition(mSwapChainBuffer[mCurrBackBuffer],D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D12_CPU_DESCRIPTOR_HANDLE view = CurrentBackBufferView();


	D3D12_CPU_DESCRIPTOR_HANDLE v = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	mDirectCmdListAlloc->Reset();

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	mCommandList->Reset(mDirectCmdListAlloc, nullptr);

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &bar);

	// Set the viewport and scissor rect.  This needs to be reset whenever the command list is reset.
	mCommandList->RSSetViewports(1, &mScreenViewport);
	mCommandList->RSSetScissorRects(1, &mScissorRect);

	// Clear the back buffer and depth buffer.
	mCommandList->ClearRenderTargetView(view, Maroon, 0, nullptr);
	mCommandList->ClearDepthStencilView(v, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	// Specify the buffers we are going to render to.
	mCommandList->OMSetRenderTargets(1, &view, true, &v);

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &bar);

	// Done recording commands.
	mCommandList->Close();

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// swap the back and front buffers
	mSwapChain->Present(0, 0);
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	// Wait until frame commands are complete.  This waiting is inefficient and is
	// done for simplicity.  Later we will show how to organize our rendering code
	// so we do not have to wait per frame.
	FlushCommandQueue();
}
 int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
	 
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = MsgProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = mhAppInst;
	wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";
	prevInstance = 0;

	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT R = { 0, 0, mClientWidth, mClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;

	mhMainWnd = CreateWindow(L"MainWnd", L"MainWndCaption",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, mhAppInst, 0);
	if (!mhMainWnd)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}
	
	ShowWindow(mhMainWnd, SW_SHOW);
	
	UpdateWindow(mhMainWnd);
	ID3D12Debug* debugController;
	D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
	debugController->EnableDebugLayer();
	CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory));

	HRESULT hardwareResult = D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&md3dDevice));

	if (FAILED(hardwareResult))
	{
		IDXGIAdapter* pWarpAdapter;
		mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter));

		D3D12CreateDevice(pWarpAdapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&md3dDevice));
	}
	md3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&mFence));
	mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = mBackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	md3dDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&msQualityLevels,
		sizeof(msQualityLevels));

	m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");

	// debug si ca marche pas;

	//Create Command Object;
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	md3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue));

	md3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&mDirectCmdListAlloc));

	md3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc, // Associated command allocator
		nullptr,                   // Initial PipelineStateObject
		IID_PPV_ARGS(&mCommandList));

	// Start off in a closed state.  This is because the first time we refer 
	// to the command list we will Reset it, and it needs to be closed before
	// calling Reset.
	mCommandList->Close();
	
	//Create Swap Chain
	mSwapChain = nullptr;

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	sd.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = mhMainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// Note: Swap chain uses queue to perform flush.
	mdxgiFactory->CreateSwapChain(mCommandQueue,&sd,&mSwapChain);

	//Create Descriptors Heaps
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	md3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap));
	
	
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;{}
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	md3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap));
	OnResize();
	MSG msg = { 0 };

	while (msg.message != WM_QUIT)
	{   // le peek doit etre dans une boucle pour traiter tous les messages en attente avant de passer à votre frame
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		UpdateWindow(mhMainWnd);
		Draw(mTimer);
	}

	return 0;

}

