#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <directxcolors.h>
#include <vector>

// timers
#include <chrono>
#include <sstream>
#include <iomanip>
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global vars
const int gWidth = 600;
const int gHeight = 450;

HINSTANCE ghInst = nullptr;
HWND       ghWnd = nullptr;

ID3D11Device        *gDevice        = nullptr;
ID3D11DeviceContext *gDeviceContext = nullptr;
IDXGISwapChain      *gSwapChain     = nullptr;

ID3D11RenderTargetView *gRenderTargetView = nullptr;
ID3D11DepthStencilView *gDepthStencilView = nullptr;

ID3D11VertexShader   *gVSShader = nullptr;
ID3D11PixelShader    *gPSShader = nullptr;

ID3D11SamplerState                     *gSampler = nullptr;
ID3D11ShaderResourceView *gTexShaderResourceView = nullptr;

enum ConstanBuffer
{
	CB_Appliation,
	CB_Frame,
	CB_Object,
	NumConstantBuffers
};
ID3D11Buffer* gCBuffers[NumConstantBuffers] = { nullptr, nullptr, nullptr };
UINT gCBObjectBind = 0;
ID3D11InputLayout *gInputLayout = nullptr;

struct GeomBuf
{
	ID3D11Buffer *vertexBuffer;
	size_t       verticesCount;
} gQuad;

float gAngleAnim = 0.0f;
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define SAFE_RELEASE( p ) if (p) { p->Release(); p = nullptr; }
#define RETURN_IF_FAILED(hr) if (FAILED(hr)) { assert(false); return hr; }
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct MyVertex
{
	float x, y, z;
	float u, v;
	MyVertex(float _x, float _y, float _z, float _u, float _v) : x(_x), y(_y), z(_z), u(_u), v(_v) {}
};
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateGeometry()
{
	std::vector<MyVertex> vBuf = {
		{ -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, }, { -1.0f, 1.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 0.0f, 1.0f, 1.0f },
		{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f }, { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f }, { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f } };
	gQuad.verticesCount = vBuf.size();

	D3D11_BUFFER_DESC vbd;
	vbd.Usage = D3D11_USAGE_IMMUTABLE;
	vbd.ByteWidth = gQuad.verticesCount * sizeof(MyVertex);
	vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbd.CPUAccessFlags = 0;
	vbd.MiscFlags = 0;
	vbd.StructureByteStride = 0;

	D3D11_SUBRESOURCE_DATA vinitData;
	vinitData.SysMemPitch = 0;
	vinitData.SysMemSlicePitch = 0;
	vinitData.pSysMem = &vBuf[0];
	HRESULT hr = gDevice->CreateBuffer(&vbd, &vinitData, &gQuad.vertexBuffer);
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateCBuffers()
{
	D3D11_BUFFER_DESC cbd;
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.ByteWidth = sizeof(DirectX::XMMATRIX); // since we store matrix only in the SimpleVertexShader
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = 0;
	cbd.MiscFlags = 0;
	cbd.StructureByteStride = 0;

	HRESULT hr = gDevice->CreateBuffer(&cbd, nullptr, &gCBuffers[CB_Appliation]);
	RETURN_IF_FAILED(hr);
	hr = gDevice->CreateBuffer(&cbd, nullptr, &gCBuffers[CB_Frame]);
	RETURN_IF_FAILED(hr);
	hr = gDevice->CreateBuffer(&cbd, nullptr, &gCBuffers[CB_Object]);
	RETURN_IF_FAILED(hr);

	// fill rarely updated matrices
	DirectX::XMMATRIX projMat = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(45.0f), static_cast<float>(gWidth) / gHeight, 0.1f, 100.0f);
	gDeviceContext->UpdateSubresource(gCBuffers[CB_Appliation], 0, nullptr, &projMat, 0, 0);

	// we can track mouse updates and change view (camera) matrix accordingly
	DirectX::XMFLOAT4 vTarget(0.0f, 0.0f, 1.0f, 0.0f);
	DirectX::XMFLOAT4 vPos(0.0f, 0.0f, -5.0f, 0.0f);
	DirectX::XMFLOAT4 vUp(0.0f, 1.0f, 0.0f, 0.0f);

	DirectX::XMVECTOR target = DirectX::XMLoadFloat4(&vTarget);
	DirectX::XMVECTOR pos = DirectX::XMLoadFloat4(&vPos);
	DirectX::XMVECTOR up = DirectX::XMLoadFloat4(&vUp);
	DirectX::XMMATRIX viewMat = DirectX::XMMatrixLookAtLH(pos, target, up);
	gDeviceContext->UpdateSubresource(gCBuffers[CB_Frame], 0, nullptr, &viewMat, 0, 0);

	DirectX::XMMATRIX identMat = DirectX::XMMatrixIdentity();
	gDeviceContext->UpdateSubresource(gCBuffers[CB_Object], 0, nullptr, &identMat, 0, 0);

	return S_OK;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateInputLayout(ID3DBlob* vsCompiledCode)
{
	// create input layout for IA stage
	D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 } // 12 offset (x,y,z) from MyVertex struct
	};

	HRESULT hr = gDevice->CreateInputLayout(
		vertexDesc, ARRAYSIZE(vertexDesc), vsCompiledCode->GetBufferPointer(), vsCompiledCode->GetBufferSize(), &gInputLayout);
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT ReflectVSGlobalCBuffer(ID3DBlob* vsCompiledCode)
{
	// reflect data example
	ID3D11ShaderReflection *reflector = nullptr;
	HRESULT hr = D3DReflect(vsCompiledCode->GetBufferPointer(), vsCompiledCode->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&reflector);
	RETURN_IF_FAILED(hr);

	D3D11_SHADER_DESC desc;
	hr = reflector->GetDesc(&desc);
	assert(SUCCEEDED(hr));

	for (UINT i = 0; i < desc.ConstantBuffers; ++i)
	{
		ID3D11ShaderReflectionConstantBuffer* cbuffer = reflector->GetConstantBufferByIndex(i);
		D3D11_SHADER_BUFFER_DESC cbDesc;
		HRESULT hr = cbuffer->GetDesc(&cbDesc);
		RETURN_IF_FAILED(hr);

		if (!strcmp(cbDesc.Name, "$Globals"))
		{
			D3D11_SHADER_INPUT_BIND_DESC resDesc;
			hr = reflector->GetResourceBindingDescByName(cbDesc.Name, &resDesc);
			RETURN_IF_FAILED(hr);

			gCBObjectBind = resDesc.BindPoint;
			break;
		}
	}
	SAFE_RELEASE(reflector);

	// check https://www.geometrictools.com/GTEngine/Source/Graphics/DX11/GteHLSLShaderFactory.cpp if you want to know how to reflect other variables / types
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateVertexShaderAndInputLayout()
{
	const char vs[] =
		"	cbuffer PerApplication : register(b0)"
		"	{"
		"		float4x4 projectionMatrix;"
		"	}"

		"	cbuffer PerFrame : register(b1)"
		"	{"
		"		float4x4 viewMatrix;"
		"	}"

		"	float4x4 perObjectWorldMatrix;"

		"	struct VertexInputType"
		"	{"
		"		float4 position : POSITION;"
		"		float2 texCoord : TEXCOORD;"
		"	};"

		"	struct PixelInputType"
		"	{"
		"		float4 position : SV_POSITION;"
		"		float2 texCoord : TEXCOORD;"
		"	};"

		"	PixelInputType SimpleVertexShader(VertexInputType input)"
		"	{"
		"		PixelInputType output;"
		
		"		input.position.w = 1.0f;"
		"		output.position = mul(perObjectWorldMatrix, input.position);"
		"		output.position = mul(viewMatrix, output.position);"
		"		output.position = mul(projectionMatrix, output.position);"

		"		output.texCoord = input.texCoord;"
		"		return output;"
		"	}";

	ID3DBlob* vsCompiledCode = nullptr;
	HRESULT hr = D3DCompile(vs, ARRAYSIZE(vs), "SimpleVS", nullptr, nullptr,
		"SimpleVertexShader", "vs_4_0", 0, 0, &vsCompiledCode, nullptr);
	RETURN_IF_FAILED(hr); // check the error blob if something goes wrong

	hr = gDevice->CreateVertexShader(vsCompiledCode->GetBufferPointer(), vsCompiledCode->GetBufferSize(), nullptr, &gVSShader);
	assert(SUCCEEDED(hr));

	hr = CreateInputLayout(vsCompiledCode);
	assert(SUCCEEDED(hr));

	hr = ReflectVSGlobalCBuffer(vsCompiledCode);
	assert(SUCCEEDED(hr));

	SAFE_RELEASE(vsCompiledCode);
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreatePixelShader()
{
	const char ps[] =
		"	Texture2D defaultTexture : register(t0);"
		"	sampler linearSampler : register(s0);"

		"	struct PixelInputType"
		"	{"
		"		float4 position : SV_POSITION;"
		"		float2 texCoord : TEXCOORD;"
		"	};"

		"	float4 SimplePixelShader(PixelInputType input) : SV_TARGET"
		"	{"
		"		return defaultTexture.Sample( linearSampler, input.texCoord );"
		"	}";

	ID3DBlob* psCompiledCode = nullptr;
	HRESULT hr = D3DCompile(ps, ARRAYSIZE(ps), "SimplePS", nullptr, nullptr,
		"SimplePixelShader", "ps_4_0", 0, 0, &psCompiledCode, nullptr);
	RETURN_IF_FAILED(hr);

	hr = gDevice->CreatePixelShader(psCompiledCode->GetBufferPointer(), psCompiledCode->GetBufferSize(), nullptr, &gPSShader);
	SAFE_RELEASE(psCompiledCode);
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateDefaultTexture()
{
	// create 8x8 checker texture with 1 mip
	UINT32 iWidth = 8;
	UINT32 iHeight = 8;

	D3D11_TEXTURE2D_DESC desc;
	desc.Width = iWidth;
	desc.Height = iHeight;
	desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	UINT32 bufSize = iWidth * iHeight;
	UINT32* buf = new UINT32[bufSize];
	for (UINT32 i = 0; i < iHeight; i++)
		for (UINT32 j = 0; j < iWidth; j++)
		{
			if ((j + i) % 2)
				buf[i * iWidth + j] = 0xFFFFFFFF;
			else
				buf[i * iWidth + j] = 0x00000000;
		}

	D3D11_SUBRESOURCE_DATA mipData;
	mipData.pSysMem = static_cast<void*>(buf);
	mipData.SysMemPitch = iWidth * sizeof(UINT32);
	mipData.SysMemSlicePitch = 0;

	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = gDevice->CreateTexture2D(&desc, &mipData, &tex);
	delete[] buf;
	RETURN_IF_FAILED(hr);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	hr = gDevice->CreateShaderResourceView(tex, &srvDesc, &gTexShaderResourceView);
	SAFE_RELEASE(tex);

	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateSampler()
{
	D3D11_SAMPLER_DESC sdesc;
	sdesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	sdesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	sdesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sdesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sdesc.MinLOD = -FLT_MAX;
	sdesc.MaxLOD = FLT_MAX;
	sdesc.MipLODBias = 0.0f;
	sdesc.MaxAnisotropy = 0;

	HRESULT hr = gDevice->CreateSamplerState(&sdesc, &gSampler);
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateAndSetDS()
{
	D3D11_DEPTH_STENCIL_DESC dsDesc;
	dsDesc.DepthEnable = TRUE;
	dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
	dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsDesc.StencilEnable = FALSE;

	ID3D11DepthStencilState *dsState = nullptr;
	HRESULT hr = gDevice->CreateDepthStencilState(&dsDesc, &dsState);
	RETURN_IF_FAILED(hr);
	gDeviceContext->OMSetDepthStencilState(dsState, 0);
	SAFE_RELEASE(dsState);

	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateAndSetRS()
{
	D3D11_RASTERIZER_DESC rsDesc;
	ZeroMemory(&rsDesc, sizeof(rsDesc));
	rsDesc.FillMode = D3D11_FILL_SOLID;
	rsDesc.CullMode = D3D11_CULL_NONE; // D3D11_CULL_BACK;

	ID3D11RasterizerState *rsState = nullptr;
	HRESULT hr = gDevice->CreateRasterizerState(&rsDesc, &rsState);
	RETURN_IF_FAILED(hr);
	gDeviceContext->RSSetState(rsState);
	SAFE_RELEASE(rsState);

	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateSwapChainAndBackbuffer(HWND hWnd)
{
	// Obtain DXGI factory from device
	IDXGIFactory1* dxgiFactory = nullptr;
	IDXGIDevice* dxgiDevice = nullptr;
	HRESULT hr = gDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
	RETURN_IF_FAILED(hr);

	IDXGIAdapter* adapter = nullptr;
	hr = dxgiDevice->GetAdapter(&adapter);
	assert(SUCCEEDED(hr));

	hr = adapter->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&dxgiFactory));
	adapter->Release();
	dxgiDevice->Release();
	RETURN_IF_FAILED(hr);

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferCount = 1;
	sd.BufferDesc.Width = gWidth;
	sd.BufferDesc.Height = gHeight; // swapChain stretch buffer to the window size (but resolutions can be different)
	sd.BufferDesc.RefreshRate.Numerator = 0;
	sd.BufferDesc.RefreshRate.Denominator = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_CENTERED;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.Flags = 0;
	hr = dxgiFactory->CreateSwapChain(gDevice, &sd, &gSwapChain);
	dxgiFactory->Release();
	RETURN_IF_FAILED(hr);

	// Create a render target view
	ID3D11Texture2D* pBackBuffer = nullptr;
	hr = gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
	RETURN_IF_FAILED(hr);

	hr = gDevice->CreateRenderTargetView(pBackBuffer, nullptr, &gRenderTargetView);
	pBackBuffer->Release();
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT CreateDepthTexture()
{
	D3D11_TEXTURE2D_DESC depthStencilDesc;
	depthStencilDesc.Width = gWidth;
	depthStencilDesc.Height = gHeight;
	depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthStencilDesc.ArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
	depthStencilDesc.CPUAccessFlags = 0;
	depthStencilDesc.MiscFlags = 0;
	depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;

	ID3D11Texture2D *depthStencilBuffer;
	HRESULT hr = gDevice->CreateTexture2D(&depthStencilDesc, 0, &depthStencilBuffer);
	RETURN_IF_FAILED(hr);

	hr = gDevice->CreateDepthStencilView(depthStencilBuffer, 0, &gDepthStencilView);
	depthStencilBuffer->Release();
	return hr;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void SetupViewport()
{
	D3D11_VIEWPORT vp;
	vp.Width = static_cast<FLOAT>(gWidth);
	vp.Height = static_cast<FLOAT>(gHeight);
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	gDeviceContext->RSSetViewports(1, &vp);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT InitRender(HWND hWnd)
{
	HRESULT hr = S_OK;

	UINT createDeviceFlags = 0;
#ifdef _DEBUG
	// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	// create directx device and context
	D3D_FEATURE_LEVEL featureLevels[] =	{ D3D_FEATURE_LEVEL_10_0 };
	UINT numFeatureLevels = ARRAYSIZE(featureLevels);

	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
		featureLevels, numFeatureLevels, D3D11_SDK_VERSION, &gDevice, nullptr, &gDeviceContext);
	RETURN_IF_FAILED(hr);

	// create WHERE we are going to draw our objects
	hr = CreateSwapChainAndBackbuffer(hWnd);
	RETURN_IF_FAILED(hr);

	hr = CreateDepthTexture();
	RETURN_IF_FAILED(hr);

	gDeviceContext->OMSetRenderTargets(1, &gRenderTargetView, gDepthStencilView);

	// create WHAT we are going to draw - geometry
	hr = CreateGeometry();
	RETURN_IF_FAILED(hr);

	// create HOW we are going to draw - vertex shader, input layout, transform matrices...
	hr = CreateVertexShaderAndInputLayout();
	RETURN_IF_FAILED(hr);

	hr = CreateCBuffers();
	RETURN_IF_FAILED(hr);

	// create pixel shader, texture, sampler, depth and rasterizer states
	hr = CreatePixelShader();
	RETURN_IF_FAILED(hr);

	hr = CreateDefaultTexture();
	RETURN_IF_FAILED(hr);

	hr = CreateSampler();
	RETURN_IF_FAILED(hr);

	hr = CreateAndSetDS();
	RETURN_IF_FAILED(hr);

	hr = CreateAndSetRS();
	RETURN_IF_FAILED(hr);

	SetupViewport();
	return true;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Cleanup()
{
	for (int i = 0; i < NumConstantBuffers; i++)
		SAFE_RELEASE(gCBuffers[i]);
	SAFE_RELEASE(gSampler);
	SAFE_RELEASE(gTexShaderResourceView);
	SAFE_RELEASE(gQuad.vertexBuffer);
	SAFE_RELEASE(gInputLayout);
	SAFE_RELEASE(gVSShader);
	SAFE_RELEASE(gPSShader);
	SAFE_RELEASE(gDepthStencilView);
	SAFE_RELEASE(gRenderTargetView);
	SAFE_RELEASE(gSwapChain);
	SAFE_RELEASE(gDeviceContext);
	SAFE_RELEASE(gDevice);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void UpdatePerObjectBuffer(float updateAngle)
{
	gAngleAnim += updateAngle;
	DirectX::XMFLOAT4 vUp(0.0f, 1.0f, 0.0f, 0.0f);
	DirectX::FXMVECTOR up = DirectX::XMLoadFloat4(&vUp);
	DirectX::XMMATRIX rotateMat = DirectX::XMMatrixRotationAxis(up, gAngleAnim);
	gDeviceContext->UpdateSubresource(gCBuffers[CB_Object], 0, nullptr, &rotateMat, 0, 0);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void RenderTick()
{
	gDeviceContext->ClearRenderTargetView(gRenderTargetView, DirectX::Colors::AliceBlue);
	gDeviceContext->ClearDepthStencilView(gDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);
	gDeviceContext->VSSetShader(gVSShader, nullptr, 0);
	gDeviceContext->PSSetShader(gPSShader, nullptr, 0);

	UINT stride = sizeof(MyVertex);
	UINT offset = 0;
	gDeviceContext->IASetInputLayout(gInputLayout);
	gDeviceContext->IASetVertexBuffers(0, 1, &gQuad.vertexBuffer, &stride, &offset);
	gDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	UpdatePerObjectBuffer(0.05f - DirectX::XM_PIDIV2);
	gDeviceContext->VSSetConstantBuffers(0, 2, &gCBuffers[CB_Appliation]);
	gDeviceContext->VSSetConstantBuffers(gCBObjectBind, 1, &gCBuffers[CB_Object]);
	gDeviceContext->PSSetSamplers(0, 1, &gSampler);
	gDeviceContext->PSSetShaderResources(0, 1, &gTexShaderResourceView);
	gDeviceContext->Draw(gQuad.verticesCount, 0);

	UpdatePerObjectBuffer(DirectX::XM_PIDIV2);
	gDeviceContext->Draw(gQuad.verticesCount, 0);

	gSwapChain->Present(1, 0);
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// There are windows specific functions below
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc;
			hdc = BeginPaint(hWnd, &ps);
			EndPaint(hWnd, &ps);
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_KEYDOWN:
		{
			switch (wParam)
			{
			case 27:
				PostQuitMessage(0);
				break;
			}
		}
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow)
{
	// Register class
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, IDI_WINLOGO);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = L"WindowClass";
	wcex.hIconSm = LoadIcon(wcex.hInstance, (LPCTSTR)IDI_WINLOGO);
	if (!RegisterClassEx(&wcex))
		return E_FAIL;

	// Create window
	ghInst = hInstance;
	RECT rc = { 0, 0, gWidth, gHeight };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	ghWnd = CreateWindow(L"WindowClass", L"Minimal DX App", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);
	if (!ghWnd)
		return E_FAIL;

	ShowWindow(ghWnd, nCmdShow);
	return S_OK;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	if (FAILED(InitWindow(hInstance, nCmdShow)))
		return 0;

	if (FAILED(InitRender(ghWnd)))
	{
		assert(false);
		Cleanup();
		return 0;
	}

	__int64 countsPerSec = 0;
	QueryPerformanceFrequency((LARGE_INTEGER*)(&countsPerSec));
	float mSecondsPerCount = 1.0f / countsPerSec;

	// Main message loop
	MSG msg = { 0 };
	while (WM_QUIT != msg.message)
	{
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			__int64 mPrevTime = 0, mCurrTime = 0;

			QueryPerformanceCounter((LARGE_INTEGER*)(&mPrevTime));
			RenderTick();
			QueryPerformanceCounter((LARGE_INTEGER*)(&mCurrTime));

			float delta = max(0, (mCurrTime - mPrevTime) * mSecondsPerCount);

			int FPS = static_cast<int>(1.0f / delta);
			std::ostringstream title;
			title.precision(5);
			title << std::fixed << "Minimal DX App FPS: " << std::setw(6) << FPS;
			SetWindowTextA(ghWnd, title.str().c_str());
		}
	}

	Cleanup();

	return (int)msg.wParam;
}
