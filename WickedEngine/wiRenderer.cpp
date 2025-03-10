#include "wiRenderer.h"
#include "wiHairParticle.h"
#include "wiEmittedParticle.h"
#include "wiSprite.h"
#include "wiScene.h"
#include "wiHelper.h"
#include "wiTextureHelper.h"
#include "wiEnums.h"
#include "wiRectPacker.h"
#include "wiBacklog.h"
#include "wiProfiler.h"
#include "wiOcean.h"
#include "wiGPUSortLib.h"
#include "wiGPUBVH.h"
#include "wiJobSystem.h"
#include "wiSpinLock.h"
#include "wiEventHandler.h"
#include "wiPlatform.h"
#include "wiSheenLUT.h"
#include "wiShaderCompiler.h"
#include "wiTimer.h"
#include "wiUnorderedMap.h" // leave it here for shader dump!
#include "wiFont.h"

#include "shaders/ShaderInterop_Postprocess.h"
#include "shaders/ShaderInterop_Raytracing.h"
#include "shaders/ShaderInterop_BVH.h"
#include "shaders/ShaderInterop_DDGI.h"

#include <algorithm>
#include <atomic>

using namespace wi::primitive;
using namespace wi::graphics;
using namespace wi::enums;
using namespace wi::scene;
using namespace wi::ecs;

namespace wi::renderer
{

GraphicsDevice*& device = GetDevice();

Shader				shaders[SHADERTYPE_COUNT];
Texture				textures[TEXTYPE_COUNT];
InputLayout			inputLayouts[ILTYPE_COUNT];
RasterizerState		rasterizers[RSTYPE_COUNT];
DepthStencilState	depthStencils[DSSTYPE_COUNT];
BlendState			blendStates[BSTYPE_COUNT];
GPUBuffer			constantBuffers[CBTYPE_COUNT];
GPUBuffer			resourceBuffers[RBTYPE_COUNT];
Sampler				samplers[SAMPLER_COUNT];

std::string SHADERPATH = "shaders/";
std::string SHADERSOURCEPATH = "../WickedEngine/shaders/";

// define this to use raytracing pipeline for raytraced reflections:
//	Currently the DX12 device could crash for unknown reasons with the global root signature export
//#define RTREFLECTION_WITH_RAYTRACING_PIPELINE

static thread_local wi::vector<GPUBarrier> barrier_stack;
void barrier_stack_flush(CommandList cmd)
{
	if (barrier_stack.empty())
		return;
	device->Barrier(barrier_stack.data(), (uint32_t)barrier_stack.size(), cmd);
	barrier_stack.clear();
}

bool TRANSPARENTSHADOWSENABLED = true;
bool wireRender = false;
bool debugBoneLines = false;
bool debugPartitionTree = false;
bool debugEmitters = false;
bool freezeCullingCamera = false;
bool debugEnvProbes = false;
bool debugForceFields = false;
bool debugCameras = false;
bool gridHelper = false;
bool voxelHelper = false;
bool advancedLightCulling = true;
bool variableRateShadingClassification = false;
bool variableRateShadingClassificationDebug = false;
float GameSpeed = 1;
bool debugLightCulling = false;
bool occlusionCulling = false;
bool temporalAA = false;
bool temporalAADEBUG = false;
uint32_t raytraceBounceCount = 3;
bool raytraceDebugVisualizer = false;
bool raytracedShadows = false;
bool tessellationEnabled = true;
bool disableAlbedoMaps = false;
bool forceDiffuseLighting = false;
bool SCREENSPACESHADOWS = false;
bool SURFELGI = false;
SURFEL_DEBUG SURFELGI_DEBUG = SURFEL_DEBUG_NONE;
bool DDGI_ENABLED = false;
bool DDGI_DEBUG_ENABLED = false;
uint32_t DDGI_RAYCOUNT = 128u;
float GI_BOOST = 1.0f;
std::atomic<size_t> SHADER_ERRORS{ 0 };
std::atomic<size_t> SHADER_MISSING{ 0 };


struct VoxelizedSceneData
{
	bool enabled = false;
	uint32_t res = 128;
	float voxelsize = 1;
	XMFLOAT3 center = XMFLOAT3(0, 0, 0);
	XMFLOAT3 extents = XMFLOAT3(0, 0, 0);
	uint32_t numCones = 2;
	float rayStepSize = 0.75f;
	float maxDistance = 20.0f;
	bool secondaryBounceEnabled = false;
	bool reflectionsEnabled = true;
	bool centerChangedThisFrame = true;
	uint32_t mips = 7;
} voxelSceneData;

Texture shadowMapAtlas;
Texture shadowMapAtlas_Transparent;
RenderPass renderpass_shadowMapAtlas;
int max_shadow_resolution_2D = 1024;
int max_shadow_resolution_cube = 256;

wi::vector<std::pair<XMFLOAT4X4, XMFLOAT4>> renderableBoxes;
wi::vector<std::pair<Sphere, XMFLOAT4>> renderableSpheres;
wi::vector<std::pair<Capsule, XMFLOAT4>> renderableCapsules;
wi::vector<RenderableLine> renderableLines;
wi::vector<RenderableLine2D> renderableLines2D;
wi::vector<RenderablePoint> renderablePoints;
wi::vector<RenderableTriangle> renderableTriangles_solid;
wi::vector<RenderableTriangle> renderableTriangles_wireframe;
wi::vector<uint8_t> debugTextStorage; // A stream of DebugText struct + text characters
wi::vector<PaintRadius> paintrads;

wi::SpinLock deferredMIPGenLock;
wi::vector<std::pair<Texture, bool>> deferredMIPGens;


bool volumetric_clouds_precomputed = false;
Texture texture_shapeNoise;
Texture texture_detailNoise;
Texture texture_curlNoise;
Texture texture_weatherMap;

// A dummy luminance buffer with exposure set to 1.
// This avoids having to branch in shaders that consume the exposure value
// when eye adaption is disabled.
// It also works around an apparent bug in the drivers for certain GTX 10xx cards
// where just testing if a bindless buffer descriptor is valid requires that it is valid.
// See: https://github.com/turanszkij/WickedEngine/issues/450
GPUBuffer luminance_dummy;

// Direct reference to a renderable instance:
struct RenderBatch
{
	uint64_t data;

	inline void Create(uint32_t meshIndex, uint32_t instanceIndex, float distance)
	{
		// These asserts are a indicating if render queue limits are reached:
		assert(meshIndex < 0x00FFFFFF);
		assert(instanceIndex < 0x00FFFFFF);

		data = 0;
		data |= uint64_t(meshIndex & 0x00FFFFFF) << 40ull;
		data |= uint64_t(XMConvertFloatToHalf(distance) & 0xFFFF) << 24ull;
		data |= uint64_t(instanceIndex & 0x00FFFFFF) << 0ull;
	}

	inline float GetDistance() const
	{
		return XMConvertHalfToFloat(HALF((data >> 24ull) & 0xFFFF));
	}
	inline uint32_t GetMeshIndex() const
	{
		return (data >> 40ull) & 0x00FFFFFF;
	}
	inline uint32_t GetInstanceIndex() const
	{
		return (data >> 0ull) & 0x00FFFFFF;
	}

	// opaque sorting
	//	Priority is set to mesh index to have more instancing
	//	distance is second priority (front to back Z-buffering)
	bool operator<(const RenderBatch& other) const
	{
		return data < other.data;
	}
	// transparent sorting
	//	Priority is distance for correct alpha blending (back to front rendering)
	//	mesh index is second priority for instancing
	bool operator>(const RenderBatch& other) const
	{
		// Swap bits of meshIndex and distance to prioritize distance more
		uint64_t a_data = 0ull;
		a_data |= ((data >> 24ull) & 0xFFFF) << 48ull; // distance repack
		a_data |= ((data >> 40ull) & 0x00FFFFFF) << 24ull; // meshIndex repack
		a_data |= data & 0x00FFFFFF; // instanceIndex repack
		uint64_t b_data = 0ull;
		b_data |= ((other.data >> 24ull) & 0xFFFF) << 48ull; // distance repack
		b_data |= ((other.data >> 40ull) & 0x00FFFFFF) << 24ull; // meshIndex repack
		b_data |= other.data & 0x00FFFFFF; // instanceIndex repack
		return a_data > b_data;
	}
};

// This is a utility that points to a linear array of render batches:
struct RenderQueue
{
	wi::vector<RenderBatch> batches;

	inline void init()
	{
		batches.clear();
	}
	inline void add(uint32_t meshIndex, uint32_t instanceIndex, float distance)
	{
		batches.emplace_back().Create(meshIndex, instanceIndex, distance);
	}
	inline void sort_transparent()
	{
		std::sort(batches.begin(), batches.end(), std::greater<RenderBatch>());
	}
	inline void sort_opaque()
	{
		std::sort(batches.begin(), batches.end(), std::less<RenderBatch>());
	}
	inline bool empty() const
	{
		return batches.empty();
	}
	inline size_t size() const
	{
		return batches.size();
	}
};


const Sampler* GetSampler(SAMPLERTYPES id)
{
	return &samplers[id];
}
const Shader* GetShader(SHADERTYPE id)
{
	return &shaders[id];
}
const InputLayout* GetInputLayout(ILTYPES id)
{
	return &inputLayouts[id];
}
const RasterizerState* GetRasterizerState(RSTYPES id)
{
	return &rasterizers[id];
}
const DepthStencilState* GetDepthStencilState(DSSTYPES id)
{
	return &depthStencils[id];
}
const BlendState* GetBlendState(BSTYPES id)
{
	return &blendStates[id];
}
const GPUBuffer* GetConstantBuffer(CBTYPES id)
{
	return &constantBuffers[id];
}
const Texture* GetTexture(TEXTYPES id)
{
	return &textures[id];
}


enum OBJECTRENDERING_DOUBLESIDED
{
	OBJECTRENDERING_DOUBLESIDED_DISABLED,
	OBJECTRENDERING_DOUBLESIDED_ENABLED,
	OBJECTRENDERING_DOUBLESIDED_BACKSIDE,
	OBJECTRENDERING_DOUBLESIDED_COUNT
};
enum OBJECTRENDERING_TESSELLATION
{
	OBJECTRENDERING_TESSELLATION_DISABLED,
	OBJECTRENDERING_TESSELLATION_ENABLED,
	OBJECTRENDERING_TESSELLATION_COUNT
};
enum OBJECTRENDERING_ALPHATEST
{
	OBJECTRENDERING_ALPHATEST_DISABLED,
	OBJECTRENDERING_ALPHATEST_ENABLED,
	OBJECTRENDERING_ALPHATEST_COUNT
};
PipelineState PSO_object
	[MaterialComponent::SHADERTYPE_COUNT]
	[RENDERPASS_COUNT]
	[BLENDMODE_COUNT]
	[OBJECTRENDERING_DOUBLESIDED_COUNT]
	[OBJECTRENDERING_TESSELLATION_COUNT]
	[OBJECTRENDERING_ALPHATEST_COUNT];
PipelineState PSO_object_wire;
PipelineState PSO_object_wire_tessellation;

wi::vector<CustomShader> customShaders;
int RegisterCustomShader(const CustomShader& customShader)
{
	int result = (int)customShaders.size();
	customShaders.push_back(customShader);
	return result;
}
const wi::vector<CustomShader>& GetCustomShaders()
{
	return customShaders;
}


SHADERTYPE GetVSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest, bool transparent)
{
	SHADERTYPE realVS = VSTYPE_OBJECT_SIMPLE;

	switch (renderPass)
	{
	case RENDERPASS_MAIN:
		if (tessellation)
		{
			realVS = VSTYPE_OBJECT_COMMON_TESSELLATION;
		}
		else
		{
			realVS = VSTYPE_OBJECT_COMMON;
		}
		break;
	case RENDERPASS_PREPASS:
		if (tessellation)
		{
			if (alphatest)
			{
				realVS = VSTYPE_OBJECT_PREPASS_ALPHATEST_TESSELLATION;
			}
			else
			{
				realVS = VSTYPE_OBJECT_PREPASS_TESSELLATION;
			}
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_OBJECT_PREPASS;
			}
		}
		break;
	case RENDERPASS_ENVMAPCAPTURE:
		realVS = VSTYPE_ENVMAP;
		break;
	case RENDERPASS_SHADOW:
		if (transparent)
		{
			realVS = VSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_SHADOW;
			}
		}
		break;
	case RENDERPASS_SHADOWCUBE:
		if (transparent)
		{
			realVS = VSTYPE_SHADOWCUBEMAPRENDER_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_SHADOWCUBEMAPRENDER;
			}
		}
		break;
	case RENDERPASS_VOXELIZE:
		realVS = VSTYPE_VOXELIZER;
		break;
	}

	return realVS;
}
SHADERTYPE GetGSTYPE(RENDERPASS renderPass, bool alphatest, bool transparent)
{
	SHADERTYPE realGS = SHADERTYPE_COUNT;

	switch (renderPass)
	{
	case RENDERPASS_VOXELIZE:
		realGS = GSTYPE_VOXELIZER;
		break;
	case RENDERPASS_ENVMAPCAPTURE:
		if (device->CheckCapability(GraphicsDeviceCapability::RENDERTARGET_AND_VIEWPORT_ARRAYINDEX_WITHOUT_GS))
			break;
		realGS = GSTYPE_ENVMAP_EMULATION;
		break;
	case RENDERPASS_SHADOWCUBE:
		if (device->CheckCapability(GraphicsDeviceCapability::RENDERTARGET_AND_VIEWPORT_ARRAYINDEX_WITHOUT_GS))
			break;
		if (transparent)
		{
			realGS = GSTYPE_SHADOWCUBEMAPRENDER_TRANSPARENT_EMULATION;
		}
		else
		{
			if (alphatest)
			{
				realGS = GSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST_EMULATION;
			}
			else
			{
				realGS = GSTYPE_SHADOWCUBEMAPRENDER_EMULATION;
			}
		}
		break;
	}

	return realGS;
}
SHADERTYPE GetHSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest)
{
	if (tessellation)
	{
		switch (renderPass)
		{
		case RENDERPASS_PREPASS:
			if (alphatest)
			{
				return HSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				return HSTYPE_OBJECT_PREPASS;
			}
			break;
		case RENDERPASS_MAIN:
			return HSTYPE_OBJECT;
			break;
		}
	}

	return SHADERTYPE_COUNT;
}
SHADERTYPE GetDSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest)
{
	if (tessellation)
	{
		switch (renderPass)
		{
		case RENDERPASS_PREPASS:
			if (alphatest)
			{
				return DSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				return DSTYPE_OBJECT_PREPASS;
			}
		case RENDERPASS_MAIN:
			return DSTYPE_OBJECT;
		}
	}

	return SHADERTYPE_COUNT;
}
SHADERTYPE GetPSTYPE(RENDERPASS renderPass, bool alphatest, bool transparent, MaterialComponent::SHADERTYPE shaderType)
{
	SHADERTYPE realPS = SHADERTYPE_COUNT;

	switch (renderPass)
	{
	case RENDERPASS_MAIN:
		switch (shaderType)
		{
		case MaterialComponent::SHADERTYPE_PBR:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT : PSTYPE_OBJECT;
			break;
		case MaterialComponent::SHADERTYPE_PBR_PLANARREFLECTION:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_PLANARREFLECTION : PSTYPE_OBJECT_PLANARREFLECTION;
			break;
		case MaterialComponent::SHADERTYPE_PBR_PARALLAXOCCLUSIONMAPPING:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_POM : PSTYPE_OBJECT_POM;
			break;
		case MaterialComponent::SHADERTYPE_PBR_ANISOTROPIC:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_ANISOTROPIC : PSTYPE_OBJECT_ANISOTROPIC;
			break;
		case MaterialComponent::SHADERTYPE_PBR_CLOTH:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_CLOTH : PSTYPE_OBJECT_CLOTH;
			break;
		case MaterialComponent::SHADERTYPE_PBR_CLEARCOAT:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_CLEARCOAT : PSTYPE_OBJECT_CLEARCOAT;
			break;
		case MaterialComponent::SHADERTYPE_PBR_CLOTH_CLEARCOAT:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_CLOTH_CLEARCOAT : PSTYPE_OBJECT_CLOTH_CLEARCOAT;
			break;
		case MaterialComponent::SHADERTYPE_WATER:
			realPS = transparent ? PSTYPE_OBJECT_WATER : SHADERTYPE_COUNT;
			break;
		case MaterialComponent::SHADERTYPE_CARTOON:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_CARTOON : PSTYPE_OBJECT_CARTOON;
			break;
		case MaterialComponent::SHADERTYPE_UNLIT:
			realPS = transparent ? PSTYPE_OBJECT_TRANSPARENT_UNLIT : PSTYPE_OBJECT_UNLIT;
			break;
		default:
			break;
		}
		break;
	case RENDERPASS_PREPASS:
		if (alphatest)
		{
			realPS = PSTYPE_OBJECT_PREPASS_ALPHATEST;
		}
		else
		{
			realPS = PSTYPE_OBJECT_PREPASS;
		}
		break;
	case RENDERPASS_ENVMAPCAPTURE:
		realPS = PSTYPE_ENVMAP;
		break;
	case RENDERPASS_SHADOW:
	case RENDERPASS_SHADOWCUBE:
		if (transparent)
		{
			realPS = shaderType == MaterialComponent::SHADERTYPE_WATER ? PSTYPE_SHADOW_WATER : PSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realPS = PSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realPS = SHADERTYPE_COUNT;
			}
		}
		break;
	case RENDERPASS_VOXELIZE:
		realPS = PSTYPE_VOXELIZER;
		break;
	default:
		break;
	}

	return realPS;
}

PipelineState PSO_decal;
PipelineState PSO_occlusionquery;
PipelineState PSO_impostor[RENDERPASS_COUNT];
PipelineState PSO_impostor_wire;
PipelineState PSO_captureimpostor;

PipelineState PSO_lightvisualizer[LightComponent::LIGHTTYPE_COUNT];
PipelineState PSO_volumetriclight[LightComponent::LIGHTTYPE_COUNT];

PipelineState PSO_renderlightmap;

PipelineState PSO_lensflare;

PipelineState PSO_downsampledepthbuffer;
PipelineState PSO_deferredcomposition;
PipelineState PSO_sss_skin;
PipelineState PSO_sss_snow;
PipelineState PSO_upsample_bilateral;
PipelineState PSO_outline;


RaytracingPipelineState RTPSO_ao;
RaytracingPipelineState RTPSO_reflection;
RaytracingPipelineState RTPSO_shadow;

enum SKYRENDERING
{
	SKYRENDERING_STATIC,
	SKYRENDERING_DYNAMIC,
	SKYRENDERING_SUN,
	SKYRENDERING_ENVMAPCAPTURE_STATIC,
	SKYRENDERING_ENVMAPCAPTURE_DYNAMIC,
	SKYRENDERING_COUNT
};
PipelineState PSO_sky[SKYRENDERING_COUNT];

enum DEBUGRENDERING
{
	DEBUGRENDERING_ENVPROBE,
	DEBUGRENDERING_DDGI,
	DEBUGRENDERING_GRID,
	DEBUGRENDERING_CUBE,
	DEBUGRENDERING_LINES,
	DEBUGRENDERING_TRIANGLE_SOLID,
	DEBUGRENDERING_TRIANGLE_WIREFRAME,
	DEBUGRENDERING_EMITTER,
	DEBUGRENDERING_PAINTRADIUS,
	DEBUGRENDERING_VOXEL,
	DEBUGRENDERING_FORCEFIELD_POINT,
	DEBUGRENDERING_FORCEFIELD_PLANE,
	DEBUGRENDERING_RAYTRACE_BVH,
	DEBUGRENDERING_COUNT
};
PipelineState PSO_debug[DEBUGRENDERING_COUNT];


#if __has_include("wiShaderDump.h")
// In this case, wiShaderDump.h contains precompiled shader binary data
#include "wiShaderDump.h"
#define SHADERDUMP_ENABLED
size_t GetShaderDumpCount()
{
	return wiShaderDump::shaderdump.size();
}
#else
// In this case, shaders can only be loaded from file
size_t GetShaderDumpCount()
{
	return 0;
}
#endif // SHADERDUMP

size_t GetShaderErrorCount()
{
	return SHADER_ERRORS.load();
}
size_t GetShaderMissingCount()
{
	return SHADER_MISSING.load();
}

bool LoadShader(
	ShaderStage stage,
	Shader& shader,
	const std::string& filename,
	ShaderModel minshadermodel,
	wi::vector<std::string> permutation_defines
)
{
	std::string shaderbinaryfilename = SHADERPATH + filename;

	if (!permutation_defines.empty())
	{
		std::string ext = wi::helper::GetExtensionFromFileName(shaderbinaryfilename);
		shaderbinaryfilename = wi::helper::RemoveExtension(shaderbinaryfilename);
		for (auto& def : permutation_defines)
		{
			shaderbinaryfilename += "_" + def;
		}
		shaderbinaryfilename += "." + ext;
	}

	if (device != nullptr)
	{
#ifdef SHADERDUMP_ENABLED
		// Loading shader from precompiled dump:
		auto it = wiShaderDump::shaderdump.find(shaderbinaryfilename);
		if (it != wiShaderDump::shaderdump.end())
		{
			return device->CreateShader(stage, it->second.data, it->second.size, &shader);
		}
		else
		{
			wi::backlog::post("shader dump doesn't contain shader: " + shaderbinaryfilename, wi::backlog::LogLevel::Error);
		}
#endif // SHADERDUMP_ENABLED
	}

	wi::shadercompiler::RegisterShader(shaderbinaryfilename);

	if (wi::shadercompiler::IsShaderOutdated(shaderbinaryfilename))
	{
		wi::shadercompiler::CompilerInput input;
		input.format = device->GetShaderFormat();
		input.stage = stage;
		input.minshadermodel = minshadermodel;
		input.defines = permutation_defines;

		std::string sourcedir = SHADERSOURCEPATH;
		wi::helper::MakePathAbsolute(sourcedir);
		input.include_directories.push_back(sourcedir);
		input.shadersourcefilename = wi::helper::ReplaceExtension(sourcedir + filename, "hlsl");

		wi::shadercompiler::CompilerOutput output;
		wi::shadercompiler::Compile(input, output);

		if (output.IsValid())
		{
			wi::shadercompiler::SaveShaderAndMetadata(shaderbinaryfilename, output);

			if (!output.error_message.empty())
			{
				wi::backlog::post(output.error_message);
			}
			wi::backlog::post("shader compiled: " + shaderbinaryfilename);
			return device->CreateShader(stage, output.shaderdata, output.shadersize, &shader);
		}
		else
		{
			wi::backlog::post("shader compile FAILED: " + shaderbinaryfilename + "\n" + output.error_message, wi::backlog::LogLevel::Error);
			SHADER_ERRORS.fetch_add(1);
		}
	}

	if (device != nullptr)
	{
		wi::vector<uint8_t> buffer;
		if (wi::helper::FileRead(shaderbinaryfilename, buffer))
		{
			return device->CreateShader(stage, buffer.data(), buffer.size(), &shader);
		}
		else
		{
			SHADER_MISSING.fetch_add(1);
		}
	}

	return false;
}


void LoadShaders()
{
	wi::jobsystem::context ctx;

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		inputLayouts[ILTYPE_OBJECT_DEBUG].elements =
		{
			{ "POSITION_NORMAL_WIND",	0, MeshComponent::Vertex_POS::FORMAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
		};
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_DEBUG], "objectVS_debug.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_COMMON], "objectVS_common.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS], "objectVS_prepass.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_ALPHATEST], "objectVS_prepass_alphatest.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW], "shadowVS.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_SIMPLE], "objectVS_simple.cso");
		LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW_ALPHATEST], "shadowVS_alphatest.cso");
		LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW_TRANSPARENT], "shadowVS_transparent.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		inputLayouts[ILTYPE_VERTEXCOLOR].elements =
		{
			{ "POSITION", 0, Format::R32G32B32A32_FLOAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
			{ "TEXCOORD", 0, Format::R32G32B32A32_FLOAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
		};
		LoadShader(ShaderStage::VS, shaders[VSTYPE_VERTEXCOLOR], "vertexcolorVS.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		inputLayouts[ILTYPE_RENDERLIGHTMAP].elements =
		{
			{ "POSITION_NORMAL_WIND",		0, MeshComponent::Vertex_POS::FORMAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
			{ "ATLAS",						0, MeshComponent::Vertex_TEX::FORMAT, 1, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
		};
		LoadShader(ShaderStage::VS, shaders[VSTYPE_RENDERLIGHTMAP], "renderlightmapVS.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_COMMON_TESSELLATION], "objectVS_common_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_TESSELLATION], "objectVS_prepass_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_ALPHATEST_TESSELLATION], "objectVS_prepass_alphatest_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_SIMPLE_TESSELLATION], "objectVS_simple_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_IMPOSTOR], "impostorVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_DIRECTIONAL], "volumetriclight_directionalVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_POINT], "volumetriclight_pointVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_SPOT], "volumetriclight_spotVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT], "vSpotLightVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT], "vPointLightVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SPHERE], "sphereVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OCCLUDEE], "occludeeVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SKY], "skyVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOXELIZER], "objectVS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOXEL], "voxelVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_FORCEFIELDVISUALIZER_POINT], "forceFieldPointVisualizerVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE], "forceFieldPlaneVisualizerVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_RAYTRACE_SCREEN], "raytrace_screenVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_POSTPROCESS], "postprocessVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LENSFLARE], "lensFlareVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_DDGI_DEBUG], "ddgi_debugVS.cso"); });

	if (device->CheckCapability(GraphicsDeviceCapability::RENDERTARGET_AND_VIEWPORT_ARRAYINDEX_WITHOUT_GS))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP], "envMapVS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP_SKY], "envMap_skyVS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER], "cubeShadowVS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST], "cubeShadowVS_alphatest.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER_TRANSPARENT], "cubeShadowVS_transparent.cso"); });
	}
	else
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP], "envMapVS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP_SKY], "envMap_skyVS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER], "cubeShadowVS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST], "cubeShadowVS_alphatest_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOWCUBEMAPRENDER_TRANSPARENT], "cubeShadowVS_transparent_emulation.cso"); });

		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_ENVMAP_EMULATION], "envMapGS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_ENVMAP_SKY_EMULATION], "envMap_skyGS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_SHADOWCUBEMAPRENDER_EMULATION], "cubeShadowGS_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_SHADOWCUBEMAPRENDER_ALPHATEST_EMULATION], "cubeShadowGS_alphatest_emulation.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_SHADOWCUBEMAPRENDER_TRANSPARENT_EMULATION], "cubeShadowGS_transparent_emulation.cso"); });
	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT], "objectPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT], "objectPS_transparent.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PLANARREFLECTION], "objectPS_planarreflection.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_PLANARREFLECTION], "objectPS_transparent_planarreflection.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_POM], "objectPS_pom.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_POM], "objectPS_transparent_pom.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_ANISOTROPIC], "objectPS_anisotropic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_ANISOTROPIC], "objectPS_transparent_anisotropic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_CLOTH], "objectPS_cloth.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_CLOTH], "objectPS_transparent_cloth.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_CLEARCOAT], "objectPS_clearcoat.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_CLEARCOAT], "objectPS_transparent_clearcoat.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_CLOTH_CLEARCOAT], "objectPS_cloth_clearcoat.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_CLOTH_CLEARCOAT], "objectPS_transparent_cloth_clearcoat.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_CARTOON], "objectPS_cartoon.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_CARTOON], "objectPS_transparent_cartoon.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_UNLIT], "objectPS_unlit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_TRANSPARENT_UNLIT], "objectPS_transparent_unlit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_WATER], "objectPS_water.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR], "impostorPS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_HOLOGRAM], "objectPS_hologram.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_DEBUG], "objectPS_debug.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PAINTRADIUS], "objectPS_paintradius.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_SIMPLE], "objectPS_simple.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PREPASS], "objectPS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PREPASS_ALPHATEST], "objectPS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR_PREPASS], "impostorPS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR_SIMPLE], "impostorPS_simple.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_LIGHTVISUALIZER], "lightVisualizerPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL], "volumetricLight_DirectionalPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_POINT], "volumetricLight_PointPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_SPOT], "volumetricLight_SpotPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP], "envMapPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP_SKY_STATIC], "envMap_skyPS_static.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP_SKY_DYNAMIC], "envMap_skyPS_dynamic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_CAPTUREIMPOSTOR], "captureImpostorPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_CUBEMAP], "cubeMapPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VERTEXCOLOR], "vertexcolorPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SKY_STATIC], "skyPS_static.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SKY_DYNAMIC], "skyPS_dynamic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SUN], "sunPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_ALPHATEST], "shadowPS_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_TRANSPARENT], "shadowPS_transparent.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_WATER], "shadowPS_water.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOXELIZER], "objectPS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOXEL], "voxelPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_FORCEFIELDVISUALIZER], "forceFieldVisualizerPS.cso"); });
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_RENDERLIGHTMAP], "renderlightmapPS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_RENDERLIGHTMAP], "renderlightmapPS.cso"); });
	}
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_RAYTRACE_DEBUGBVH], "raytrace_debugbvhPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_DOWNSAMPLEDEPTHBUFFER], "downsampleDepthBuffer4xPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL], "upsample_bilateralPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_POSTPROCESS_OUTLINE], "outlinePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_LENSFLARE], "lensFlarePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_DDGI_DEBUG], "ddgi_debugPS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_VOXELIZER], "objectGS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_VOXEL], "voxelGS.cso"); });


	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LUMINANCE_PASS1], "luminancePass1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LUMINANCE_PASS2], "luminancePass2CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SHADINGRATECLASSIFICATION], "shadingRateClassificationCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SHADINGRATECLASSIFICATION_DEBUG], "shadingRateClassificationCS_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TILEFRUSTUMS], "tileFrustumsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING], "lightCullingCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_DEBUG], "lightCullingCS_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_ADVANCED], "lightCullingCS_ADVANCED.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_ADVANCED_DEBUG], "lightCullingCS_ADVANCED_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RESOLVEMSAADEPTHSTENCIL], "resolveMSAADepthStencilCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VOXELSCENECOPYCLEAR], "voxelSceneCopyClearCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VOXELSCENECOPYCLEAR_TEMPORALSMOOTHING], "voxelSceneCopyClearCS_TemporalSmoothing.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VOXELRADIANCESECONDARYBOUNCE], "voxelRadianceSecondaryBounceCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VOXELCLEARONLYNORMAL], "voxelClearOnlyNormalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_TRANSMITTANCELUT], "skyAtmosphere_transmittanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], "skyAtmosphere_multiScatteredLuminanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_SKYVIEWLUT], "skyAtmosphere_skyViewLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_SKYLUMINANCELUT], "skyAtmosphere_skyLuminanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN2D_UNORM4], "generateMIPChain2DCS_unorm4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4], "generateMIPChain2DCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN3D_UNORM4], "generateMIPChain3DCS_unorm4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4], "generateMIPChain3DCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBE_UNORM4], "generateMIPChainCubeCS_unorm4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4], "generateMIPChainCubeCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4], "generateMIPChainCubeArrayCS_unorm4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4], "generateMIPChainCubeArrayCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_FILTERENVMAP], "filterEnvMapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_UNORM4], "copytexture2D_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_FLOAT4], "copytexture2D_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_UNORM4_BORDEREXPAND], "copytexture2D_unorm4_borderexpandCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_FLOAT4_BORDEREXPAND], "copytexture2D_float4_borderexpandCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKINNING], "skinningCS.cso"); });
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RAYTRACE], "raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RAYTRACE], "raytraceCS.cso"); });
	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_PAINT_TEXTURE], "paint_textureCS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT1], "blur_gaussian_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT3], "blur_gaussian_float3CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4], "blur_gaussian_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_UNORM1], "blur_gaussian_unorm1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_UNORM4], "blur_gaussian_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT1], "blur_gaussian_wide_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT3], "blur_gaussian_wide_float3CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT4], "blur_gaussian_wide_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_UNORM1], "blur_gaussian_wide_unorm1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_UNORM4], "blur_gaussian_wide_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT1], "blur_bilateral_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT3], "blur_bilateral_float3CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4], "blur_bilateral_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_UNORM1], "blur_bilateral_unorm1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_UNORM4], "blur_bilateral_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT1], "blur_bilateral_wide_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT3], "blur_bilateral_wide_float3CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT4], "blur_bilateral_wide_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_UNORM1], "blur_bilateral_wide_unorm1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_UNORM4], "blur_bilateral_wide_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSAO], "ssaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_HBAO], "hbaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS1], "msao_preparedepthbuffers1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS2], "msao_preparedepthbuffers2CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_INTERLEAVE], "msao_interleaveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO], "msaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE], "msao_blurupsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_BLENDOUT], "msao_blurupsampleCS_blendout.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN], "msao_blurupsampleCS_premin.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN_BLENDOUT], "msao_blurupsampleCS_premin_blendout.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_HORIZONTAL], "ssr_tileMaxRoughness_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_VERTICAL], "ssr_tileMaxRoughness_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_KICKJOBS], "ssr_kickjobsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_DEPTHHIERARCHY], "ssr_depthHierarchyCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE], "ssr_raytraceCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_EARLYEXIT], "ssr_raytraceCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_CHEAP], "ssr_raytraceCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], "ssr_resolveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], "ssr_temporalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_BILATERAL], "ssr_bilateralCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_LIGHTSHAFTS], "lightShaftsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_HORIZONTAL], "depthoffield_tileMaxCOC_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_VERTICAL], "depthoffield_tileMaxCOC_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_NEIGHBORHOODMAXCOC], "depthoffield_neighborhoodMaxCOCCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_KICKJOBS], "depthoffield_kickjobsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], "depthoffield_prepassCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS_EARLYEXIT], "depthoffield_prepassCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN], "depthoffield_mainCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_EARLYEXIT], "depthoffield_mainCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_CHEAP], "depthoffield_mainCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_POSTFILTER], "depthoffield_postfilterCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_UPSAMPLE], "depthoffield_upsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_HORIZONTAL], "motionblur_tileMaxVelocity_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_VERTICAL], "motionblur_tileMaxVelocity_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_NEIGHBORHOODMAXVELOCITY], "motionblur_neighborhoodMaxVelocityCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_KICKJOBS], "motionblur_kickjobsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR], "motionblurCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_EARLYEXIT], "motionblurCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_CHEAP], "motionblurCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLOOMSEPARATE], "bloomseparateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHAPENOISE], "volumetricCloud_shapenoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_DETAILNOISE], "volumetricCloud_detailnoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_CURLNOISE], "volumetricCloud_curlnoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_WEATHERMAP], "volumetricCloud_weathermapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER], "volumetricCloud_renderCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_REPROJECT], "volumetricCloud_reprojectCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_TEMPORAL], "volumetricCloud_temporalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FXAA], "fxaaCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_TEMPORALAA], "temporalaaCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SHARPEN], "sharpenCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_TONEMAP], "tonemapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UNDERWATER], "underwaterCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR_UPSCALING], "fsr_upscalingCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR_SHARPEN], "fsr_sharpenCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_CHROMATIC_ABERRATION], "chromatic_aberrationCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT1], "upsample_bilateral_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UNORM1], "upsample_bilateral_unorm1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4], "upsample_bilateral_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UNORM4], "upsample_bilateral_unorm4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UINT4], "upsample_bilateral_uint4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DOWNSAMPLE4X], "downsample4xCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_NORMALSFROMDEPTH], "normalsfromdepthCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SCREENSPACESHADOW], "screenspaceshadowCS.cso"); });

	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTREFLECTION], "rtreflectionCS.cso", ShaderModel::SM_6_5); });

		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW], "rtshadowCS.cso", ShaderModel::SM_6_5); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TILECLASSIFICATION], "rtshadow_denoise_tileclassificationCS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_FILTER], "rtshadow_denoise_filterCS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TEMPORAL], "rtshadow_denoise_temporalCS.cso"); });

		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO], "rtaoCS.cso", ShaderModel::SM_6_5); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_TILECLASSIFICATION], "rtao_denoise_tileclassificationCS.cso"); });
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_FILTER], "rtao_denoise_filterCS.cso"); });

	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_COVERAGE], "surfel_coverageCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_INDIRECTPREPARE], "surfel_indirectprepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_UPDATE], "surfel_updateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_GRIDRESET], "surfel_gridresetCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_GRIDOFFSETS], "surfel_gridoffsetsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_BINNING], "surfel_binningCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_INTEGRATE], "surfel_integrateCS.cso"); });
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_RAYTRACE], "surfel_raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_RAYTRACE], "surfel_raytraceCS.cso"); });
	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_RESOLVE], "visibility_resolveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_RESOLVE_MSAA], "visibility_resolveCS_MSAA.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_INDIRECT_PREPARE], "visibility_indirect_prepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_SKY], "visibility_skyCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_VELOCITY], "visibility_velocityCS.cso"); });

	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_RAYTRACE], "ddgi_raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_RAYTRACE], "ddgi_raytraceCS.cso"); });
	}
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_UPDATE], "ddgi_updateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_UPDATE_DEPTH], "ddgi_updateCS_depth.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TERRAIN_VIRTUALTEXTURE_UPDATE], "terrainVirtualTextureUpdateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_MESHLET_PREPARE], "meshlet_prepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_IMPOSTOR_PREPARE], "impostor_prepareCS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT], "objectHS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_PREPASS], "objectHS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_PREPASS_ALPHATEST], "objectHS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_SIMPLE], "objectHS_simple.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT], "objectDS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_PREPASS], "objectDS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_PREPASS_ALPHATEST], "objectDS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_SIMPLE], "objectDS_simple.cso"); });

	wi::jobsystem::Wait(ctx);

	// default objectshaders:
	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		MaterialComponent::SHADERTYPE shaderType = (MaterialComponent::SHADERTYPE)args.jobIndex;

		for (int renderPass = 0; renderPass < RENDERPASS_COUNT; ++renderPass)
		{
			for (int blendMode = 0; blendMode < BLENDMODE_COUNT; ++blendMode)
			{
				for (int doublesided = 0; doublesided < OBJECTRENDERING_DOUBLESIDED_COUNT; ++doublesided)
				{
					for (int tessellation = 0; tessellation < OBJECTRENDERING_TESSELLATION_COUNT; ++tessellation)
					{
						for (int alphatest = 0; alphatest < OBJECTRENDERING_ALPHATEST_COUNT; ++alphatest)
						{
							const bool transparency = blendMode != BLENDMODE_OPAQUE;
							SHADERTYPE realVS = GetVSTYPE((RENDERPASS)renderPass, tessellation, alphatest, transparency);
							SHADERTYPE realHS = GetHSTYPE((RENDERPASS)renderPass, tessellation, alphatest);
							SHADERTYPE realDS = GetDSTYPE((RENDERPASS)renderPass, tessellation, alphatest);
							SHADERTYPE realGS = GetGSTYPE((RENDERPASS)renderPass, alphatest, transparency);
							SHADERTYPE realPS = GetPSTYPE((RENDERPASS)renderPass, alphatest, transparency, shaderType);

							if (tessellation && (realHS == SHADERTYPE_COUNT || realDS == SHADERTYPE_COUNT))
							{
								continue;
							}

							PipelineStateDesc desc;
							desc.vs = realVS < SHADERTYPE_COUNT ? &shaders[realVS] : nullptr;
							desc.hs = realHS < SHADERTYPE_COUNT ? &shaders[realHS] : nullptr;
							desc.ds = realDS < SHADERTYPE_COUNT ? &shaders[realDS] : nullptr;
							desc.gs = realGS < SHADERTYPE_COUNT ? &shaders[realGS] : nullptr;
							desc.ps = realPS < SHADERTYPE_COUNT ? &shaders[realPS] : nullptr;

							switch (blendMode)
							{
							case BLENDMODE_OPAQUE:
								desc.bs = &blendStates[BSTYPE_OPAQUE];
								break;
							case BLENDMODE_ALPHA:
								desc.bs = &blendStates[BSTYPE_TRANSPARENT];
								break;
							case BLENDMODE_ADDITIVE:
								desc.bs = &blendStates[BSTYPE_ADDITIVE];
								break;
							case BLENDMODE_PREMULTIPLIED:
								desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
								break;
							case BLENDMODE_MULTIPLY:
								desc.bs = &blendStates[BSTYPE_MULTIPLY];
								break;
							default:
								assert(0);
								break;
							}

							switch (renderPass)
							{
							case RENDERPASS_SHADOW:
							case RENDERPASS_SHADOWCUBE:
								desc.bs = &blendStates[transparency ? BSTYPE_TRANSPARENTSHADOW : BSTYPE_COLORWRITEDISABLE];
								break;
							default:
								break;
							}

							switch (renderPass)
							{
							case RENDERPASS_SHADOW:
							case RENDERPASS_SHADOWCUBE:
								desc.dss = &depthStencils[transparency ? DSSTYPE_DEPTHREAD : DSSTYPE_SHADOW];
								break;
							case RENDERPASS_MAIN:
								if (blendMode == BLENDMODE_ADDITIVE)
								{
									desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
								}
								else
								{
									desc.dss = &depthStencils[transparency ? DSSTYPE_DEFAULT : DSSTYPE_DEPTHREADEQUAL];
								}
								break;
							case RENDERPASS_ENVMAPCAPTURE:
								desc.dss = &depthStencils[DSSTYPE_ENVMAP];
								break;
							case RENDERPASS_VOXELIZE:
								desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
								break;
							default:
								if (blendMode == BLENDMODE_ADDITIVE)
								{
									desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
								}
								else
								{
									desc.dss = &depthStencils[DSSTYPE_DEFAULT];
								}
								break;
							}

							switch (renderPass)
							{
							case RENDERPASS_SHADOW:
							case RENDERPASS_SHADOWCUBE:
								desc.rs = &rasterizers[doublesided ? RSTYPE_SHADOW_DOUBLESIDED : RSTYPE_SHADOW];
								break;
							case RENDERPASS_VOXELIZE:
								desc.rs = &rasterizers[RSTYPE_VOXELIZE];
								break;
							default:
								switch (doublesided)
								{
								default:
								case OBJECTRENDERING_DOUBLESIDED_DISABLED:
									desc.rs = &rasterizers[RSTYPE_FRONT];
									break;
								case OBJECTRENDERING_DOUBLESIDED_ENABLED:
									desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
									break;
								case OBJECTRENDERING_DOUBLESIDED_BACKSIDE:
									desc.rs = &rasterizers[RSTYPE_BACK];
									break;
								}
								break;
							}

							if (tessellation)
							{
								desc.pt = PrimitiveTopology::PATCHLIST;
							}
							else
							{
								desc.pt = PrimitiveTopology::TRIANGLELIST;
							}

							device->CreatePipelineState(&desc, &PSO_object[shaderType][renderPass][blendMode][doublesided][tessellation][alphatest]);
						}
					}
				}
			}
		}
	});

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SURFACE_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_surfaceCS.cso",
			ShaderModel::SM_6_0,
			MaterialComponent::shaderTypeDefines[args.jobIndex] // permutation defines
		);

		});

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		auto defines = MaterialComponent::shaderTypeDefines[args.jobIndex];
		defines.push_back("REDUCED");
		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SURFACE_REDUCED_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_surfaceCS.cso",
			ShaderModel::SM_6_0,
			defines // permutation defines
		);

		});

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SHADE_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_shadeCS.cso",
			ShaderModel::SM_6_0,
			MaterialComponent::shaderTypeDefines[args.jobIndex] // permutation defines
		);

		});

	// Clear custom shaders (Custom shaders coming from user will need to be handled by the user in case of shader reload):
	customShaders.clear();

	// Hologram sample shader will be registered as custom shader:
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		SHADERTYPE realVS = GetVSTYPE(RENDERPASS_MAIN, false, false, true);

		PipelineStateDesc desc;
		desc.vs = &shaders[realVS];
		desc.ps = &shaders[PSTYPE_OBJECT_HOLOGRAM];

		desc.bs = &blendStates[BSTYPE_ADDITIVE];
		desc.rs = &rasterizers[RSTYPE_FRONT];
		desc.dss = &depthStencils[DSSTYPE_HOLOGRAM];
		desc.pt = PrimitiveTopology::TRIANGLELIST;

		PipelineState pso;
		device->CreatePipelineState(&desc, &pso);

		CustomShader customShader;
		customShader.name = "Hologram";
		customShader.renderTypeFlags = RENDERTYPE_TRANSPARENT;
		customShader.pso[RENDERPASS_MAIN] = pso;
		RegisterCustomShader(customShader);
		});


	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE];
		desc.ps = &shaders[PSTYPE_OBJECT_SIMPLE];
		desc.rs = &rasterizers[RSTYPE_WIRE];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];

		device->CreatePipelineState(&desc, &PSO_object_wire);

		desc.pt = PrimitiveTopology::PATCHLIST;
		desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE_TESSELLATION];
		desc.hs = &shaders[HSTYPE_OBJECT_SIMPLE];
		desc.ds = &shaders[DSTYPE_OBJECT_SIMPLE];
		device->CreatePipelineState(&desc, &PSO_object_wire_tessellation);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OCCLUDEE];
		desc.rs = &rasterizers[RSTYPE_OCCLUDEE];
		desc.bs = &blendStates[BSTYPE_COLORWRITEDISABLE];
		desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
		desc.pt = PrimitiveTopology::TRIANGLESTRIP;

		device->CreatePipelineState(&desc, &PSO_occlusionquery);
		});
	wi::jobsystem::Dispatch(ctx, RENDERPASS_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		const bool impostorRequest =
			args.jobIndex != RENDERPASS_VOXELIZE &&
			args.jobIndex != RENDERPASS_SHADOW &&
			args.jobIndex != RENDERPASS_SHADOWCUBE &&
			args.jobIndex != RENDERPASS_ENVMAPCAPTURE;
		if (!impostorRequest)
		{
			return;
		}

		PipelineStateDesc desc;
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];
		desc.il = nullptr;

		switch (args.jobIndex)
		{
		case RENDERPASS_MAIN:
			desc.dss = &depthStencils[DSSTYPE_DEPTHREADEQUAL];
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR];
			break;
		case RENDERPASS_PREPASS:
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR_PREPASS];
			break;
		default:
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR_PREPASS];
			break;
		}

		device->CreatePipelineState(&desc, &PSO_impostor[args.jobIndex]);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_IMPOSTOR];
		desc.ps = &shaders[PSTYPE_IMPOSTOR_SIMPLE];
		desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];
		desc.il = nullptr;

		device->CreatePipelineState(&desc, &PSO_impostor_wire);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OBJECT_COMMON];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_CAPTUREIMPOSTOR];

		desc.ps = &shaders[PSTYPE_CAPTUREIMPOSTOR];
		device->CreatePipelineState(&desc, &PSO_captureimpostor);
		});

	wi::jobsystem::Dispatch(ctx, LightComponent::LIGHTTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		// deferred lights:

		desc.pt = PrimitiveTopology::TRIANGLELIST;


		// light visualizers:
		if (args.jobIndex != LightComponent::DIRECTIONAL)
		{

			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.ps = &shaders[PSTYPE_LIGHTVISUALIZER];

			switch (args.jobIndex)
			{
			case LightComponent::POINT:
				desc.bs = &blendStates[BSTYPE_ADDITIVE];
				desc.vs = &shaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT];
				desc.rs = &rasterizers[RSTYPE_FRONT];
				break;
			case LightComponent::SPOT:
				desc.bs = &blendStates[BSTYPE_ADDITIVE];
				desc.vs = &shaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT];
				desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
				break;
			}

			device->CreatePipelineState(&desc, &PSO_lightvisualizer[args.jobIndex]);
		}


		// volumetric lights:
		if (args.jobIndex <= LightComponent::SPOT)
		{
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.rs = &rasterizers[RSTYPE_BACK];

			switch (args.jobIndex)
			{
			case LightComponent::DIRECTIONAL:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_DIRECTIONAL];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL];
				break;
			case LightComponent::POINT:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_POINT];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_POINT];
				break;
			case LightComponent::SPOT:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_SPOT];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_SPOT];
				break;
			}

			device->CreatePipelineState(&desc, &PSO_volumetriclight[args.jobIndex]);
		}


		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.il = &inputLayouts[ILTYPE_RENDERLIGHTMAP];
		desc.vs = &shaders[VSTYPE_RENDERLIGHTMAP];
		desc.ps = &shaders[PSTYPE_RENDERLIGHTMAP];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_TRANSPARENT];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_renderlightmap);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_DOWNSAMPLEDEPTHBUFFER];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_WRITEONLY];

		device->CreatePipelineState(&desc, &PSO_downsampledepthbuffer);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_upsample_bilateral);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_POSTPROCESS_OUTLINE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_TRANSPARENT];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_outline);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_LENSFLARE];
		desc.ps = &shaders[PSTYPE_LENSFLARE];
		desc.bs = &blendStates[BSTYPE_ADDITIVE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
		desc.pt = PrimitiveTopology::TRIANGLESTRIP;

		device->CreatePipelineState(&desc, &PSO_lensflare);
		});
	wi::jobsystem::Dispatch(ctx, SKYRENDERING_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.rs = &rasterizers[RSTYPE_SKY];
		desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];

		switch (args.jobIndex)
		{
		case SKYRENDERING_STATIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SKY_STATIC];
			break;
		case SKYRENDERING_DYNAMIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SKY_DYNAMIC];
			break;
		case SKYRENDERING_SUN:
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SUN];
			break;
		case SKYRENDERING_ENVMAPCAPTURE_STATIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_ENVMAP_SKY];
			desc.ps = &shaders[PSTYPE_ENVMAP_SKY_STATIC];
			if (!device->CheckCapability(GraphicsDeviceCapability::RENDERTARGET_AND_VIEWPORT_ARRAYINDEX_WITHOUT_GS))
			{
				desc.gs = &shaders[GSTYPE_ENVMAP_SKY_EMULATION];
			}
			break;
		case SKYRENDERING_ENVMAPCAPTURE_DYNAMIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_ENVMAP_SKY];
			desc.ps = &shaders[PSTYPE_ENVMAP_SKY_DYNAMIC];
			if (!device->CheckCapability(GraphicsDeviceCapability::RENDERTARGET_AND_VIEWPORT_ARRAYINDEX_WITHOUT_GS))
			{
				desc.gs = &shaders[GSTYPE_ENVMAP_SKY_EMULATION];
			}
			break;
		}

		device->CreatePipelineState(&desc, &PSO_sky[args.jobIndex]);
		});
	wi::jobsystem::Dispatch(ctx, DEBUGRENDERING_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		switch (args.jobIndex)
		{
		case DEBUGRENDERING_ENVPROBE:
			desc.vs = &shaders[VSTYPE_SPHERE];
			desc.ps = &shaders[PSTYPE_CUBEMAP];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_DDGI:
			desc.vs = &shaders[VSTYPE_DDGI_DEBUG];
			desc.ps = &shaders[PSTYPE_DDGI_DEBUG];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_GRID:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_CUBE:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_LINES:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_SOLID:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_WIREFRAME:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_EMITTER:
			desc.vs = &shaders[VSTYPE_OBJECT_DEBUG];
			desc.ps = &shaders[PSTYPE_OBJECT_DEBUG];
			desc.il = &inputLayouts[ILTYPE_OBJECT_DEBUG];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_PAINTRADIUS:
			desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE];
			desc.ps = &shaders[PSTYPE_OBJECT_PAINTRADIUS];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_VOXEL:
			desc.vs = &shaders[VSTYPE_VOXEL];
			desc.ps = &shaders[PSTYPE_VOXEL];
			desc.gs = &shaders[GSTYPE_VOXEL];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_BACK];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::POINTLIST;
			break;
		case DEBUGRENDERING_FORCEFIELD_POINT:
			desc.vs = &shaders[VSTYPE_FORCEFIELDVISUALIZER_POINT];
			desc.ps = &shaders[PSTYPE_FORCEFIELDVISUALIZER];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_BACK];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_FORCEFIELD_PLANE:
			desc.vs = &shaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE];
			desc.ps = &shaders[PSTYPE_FORCEFIELDVISUALIZER];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.pt = PrimitiveTopology::TRIANGLESTRIP;
			break;
		case DEBUGRENDERING_RAYTRACE_BVH:
			desc.vs = &shaders[VSTYPE_RAYTRACE_SCREEN];
			desc.ps = &shaders[PSTYPE_RAYTRACE_DEBUGBVH];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		}

		device->CreatePipelineState(&desc, &PSO_debug[args.jobIndex]);
		});

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
	if(device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {

			bool success = LoadShader(ShaderStage::LIB, shaders[RTTYPE_RTREFLECTION], "rtreflectionLIB.cso");
			assert(success);

			RaytracingPipelineStateDesc rtdesc;
			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_Raygen";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::RAYGENERATION;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_ClosestHit";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::CLOSESTHIT;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_AnyHit";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::ANYHIT;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_Miss";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::MISS;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::GENERAL;
			rtdesc.hit_groups.back().name = "RTReflection_Raygen";
			rtdesc.hit_groups.back().general_shader = 0;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::GENERAL;
			rtdesc.hit_groups.back().name = "RTReflection_Miss";
			rtdesc.hit_groups.back().general_shader = 3;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::TRIANGLES;
			rtdesc.hit_groups.back().name = "RTReflection_Hitgroup";
			rtdesc.hit_groups.back().closest_hit_shader = 1;
			rtdesc.hit_groups.back().any_hit_shader = 2;

			rtdesc.max_trace_recursion_depth = 1;
			rtdesc.max_payload_size_in_bytes = sizeof(float4);
			rtdesc.max_attribute_size_in_bytes = sizeof(float2); // bary
			success = device->CreateRaytracingPipelineState(&rtdesc, &RTPSO_reflection);


		});
	};
#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

	wi::jobsystem::Wait(ctx);

}
void LoadBuffers()
{
	GPUBufferDesc bd;

	// The following buffers will be DEFAULT (long lifetime, slow update, fast read):
	bd.usage = Usage::DEFAULT;

	bd.size = sizeof(FrameCB);
	bd.bind_flags = BindFlag::CONSTANT_BUFFER;
	device->CreateBuffer(&bd, nullptr, &constantBuffers[CBTYPE_FRAME]);
	device->SetName(&constantBuffers[CBTYPE_FRAME], "constantBuffers[CBTYPE_FRAME]");


	bd.size = sizeof(ShaderEntity) * SHADER_ENTITY_COUNT;
	bd.bind_flags = BindFlag::SHADER_RESOURCE;
	bd.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	bd.stride = sizeof(ShaderEntity);
	device->CreateBuffer(&bd, nullptr, &resourceBuffers[RBTYPE_ENTITYARRAY]);
	device->SetName(&resourceBuffers[RBTYPE_ENTITYARRAY], "resourceBuffers[RBTYPE_ENTITYARRAY]");

	bd.size = sizeof(XMMATRIX) * MATRIXARRAY_COUNT;
	bd.bind_flags = BindFlag::SHADER_RESOURCE;
	bd.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	bd.stride = sizeof(XMMATRIX);
	device->CreateBuffer(&bd, nullptr, &resourceBuffers[RBTYPE_MATRIXARRAY]);
	device->SetName(&resourceBuffers[RBTYPE_MATRIXARRAY], "resourceBuffers[RBTYPE_MATRIXARRAY]");

	{
		TextureDesc desc;
		desc.bind_flags = BindFlag::SHADER_RESOURCE;
		desc.format = Format::R8_UNORM;
		desc.height = 16;
		desc.width = 16;
		SubresourceData InitData;
		InitData.data_ptr = sheenLUTdata;
		InitData.row_pitch = desc.width;
		device->CreateTexture(&desc, &InitData, &textures[TEXTYPE_2D_SHEENLUT]);
		device->SetName(&textures[TEXTYPE_2D_SHEENLUT], "textures[TEXTYPE_2D_SHEENLUT]");
	}

	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 256;
		desc.height = 64;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT]");
	}
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 32;
		desc.height = 32;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT]");
	}
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 192;
		desc.height = 104;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT]");
	}
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 1;
		desc.height = 1;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT]");
	}
	{
		// the dummy buffer is read-only so only the first 'exposure' value is needed,
		// not the luminance or histogram values in the full version of the buffer used
        // when eye adaption is enabled.
		float values[1] = { 1 };

		GPUBufferDesc desc;
		desc.size = sizeof(values);
		desc.bind_flags = BindFlag::SHADER_RESOURCE;
		desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
		device->CreateBuffer(&desc, values, &luminance_dummy);
		device->SetName(&luminance_dummy, "luminance_dummy");

		static_assert(LUMINANCE_BUFFER_OFFSET_EXPOSURE == 0);
	}
}
void SetUpStates()
{
	RasterizerState rs;
	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_FRONT] = rs;


	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	rs.depth_bias = -1;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = -4.0f;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_SHADOW] = rs;
	rs.cull_mode = CullMode::NONE;
	rasterizers[RSTYPE_SHADOW_DOUBLESIDED] = rs;

	rs.fill_mode = FillMode::WIREFRAME;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_WIRE] = rs;
	rs.antialiased_line_enable = true;
	rasterizers[RSTYPE_WIRE_SMOOTH] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_DOUBLESIDED] = rs;

	rs.fill_mode = FillMode::WIREFRAME;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_WIRE_DOUBLESIDED] = rs;
	rs.antialiased_line_enable = true;
	rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::FRONT;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_BACK] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 1;
	rs.depth_bias_clamp = 0.01f;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = false;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_OCCLUDEE] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::FRONT;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = false;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_SKY] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rs.forced_sample_count = 8;
	rasterizers[RSTYPE_VOXELIZE] = rs;




	DepthStencilState dsd;
	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;

	dsd.stencil_enable = true;
	dsd.stencil_read_mask = 0;
	dsd.stencil_write_mask = 0xFF;
	dsd.front_face.stencil_func = ComparisonFunc::ALWAYS;
	dsd.front_face.stencil_pass_op = StencilOp::REPLACE;
	dsd.front_face.stencil_fail_op = StencilOp::KEEP;
	dsd.front_face.stencil_depth_fail_op = StencilOp::KEEP;
	dsd.back_face.stencil_func = ComparisonFunc::ALWAYS;
	dsd.back_face.stencil_pass_op = StencilOp::REPLACE;
	dsd.back_face.stencil_fail_op = StencilOp::KEEP;
	dsd.back_face.stencil_depth_fail_op = StencilOp::KEEP;
	depthStencils[DSSTYPE_DEFAULT] = dsd;

	dsd.depth_write_mask = DepthWriteMask::ZERO;
	depthStencils[DSSTYPE_HOLOGRAM] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_SHADOW] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_CAPTUREIMPOSTOR] = dsd;


	dsd.depth_enable = true;
	dsd.stencil_enable = false;
	dsd.depth_write_mask = DepthWriteMask::ZERO;
	dsd.depth_func = ComparisonFunc::GREATER_EQUAL;
	depthStencils[DSSTYPE_DEPTHREAD] = dsd;

	dsd.depth_enable = false;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_DEPTHDISABLED] = dsd;


	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ZERO;
	dsd.depth_func = ComparisonFunc::EQUAL;
	depthStencils[DSSTYPE_DEPTHREADEQUAL] = dsd;


	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	depthStencils[DSSTYPE_ENVMAP] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::ALWAYS;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_WRITEONLY] = dsd;


	BlendState bd;
	bd.render_target[0].blend_enable = false;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_OPAQUE] = bd;

	bd.render_target[0].src_blend = Blend::SRC_ALPHA;
	bd.render_target[0].dest_blend = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_TRANSPARENT] = bd;

	bd.render_target[0].blend_enable = true;
	bd.render_target[0].src_blend = Blend::ONE;
	bd.render_target[0].dest_blend = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.independent_blend_enable = false;
	bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_PREMULTIPLIED] = bd;


	bd.render_target[0].blend_enable = true;
	bd.render_target[0].src_blend = Blend::SRC_ALPHA;
	bd.render_target[0].dest_blend = Blend::ONE;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ZERO;
	bd.render_target[0].dest_blend_alpha = Blend::ONE;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.independent_blend_enable = false,
		bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_ADDITIVE] = bd;


	bd.render_target[0].blend_enable = false;
	bd.render_target[0].render_target_write_mask = ColorWrite::DISABLE;
	bd.independent_blend_enable = false,
		bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_COLORWRITEDISABLE] = bd;

	bd.render_target[0].src_blend = Blend::DEST_COLOR;
	bd.render_target[0].dest_blend = Blend::ZERO;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::DEST_ALPHA;
	bd.render_target[0].dest_blend_alpha = Blend::ZERO;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_MULTIPLY] = bd;

	bd.render_target[0].src_blend = Blend::ZERO;
	bd.render_target[0].dest_blend = Blend::SRC_COLOR;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::ONE;
	bd.render_target[0].blend_op_alpha = BlendOp::MAX;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_TRANSPARENTSHADOW] = bd;





	SamplerDesc samplerDesc;
	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	samplerDesc.mip_lod_bias = 0.0f;
	samplerDesc.max_anisotropy = 0;
	samplerDesc.comparison_func = ComparisonFunc::NEVER;
	samplerDesc.border_color = SamplerBorderColor::TRANSPARENT_BLACK;
	samplerDesc.min_lod = 0;
	samplerDesc.max_lod = std::numeric_limits<float>::max();
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_MIRROR]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_CLAMP]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_WRAP]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_MIRROR]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_WRAP]);


	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_CLAMP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_CLAMP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_WRAP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_MIRROR]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_OBJECTSHADER]);

	samplerDesc.filter = Filter::COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	samplerDesc.mip_lod_bias = 0.0f;
	samplerDesc.max_anisotropy = 0;
	samplerDesc.comparison_func = ComparisonFunc::GREATER_EQUAL;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_CMP_DEPTH]);
}

void ModifyObjectSampler(const SamplerDesc& desc)
{
	device->CreateSampler(&desc, &samplers[SAMPLER_OBJECTSHADER]);
}

const std::string& GetShaderPath()
{
	return SHADERPATH;
}
void SetShaderPath(const std::string& path)
{
	SHADERPATH = path;
}
const std::string& GetShaderSourcePath()
{
	return SHADERSOURCEPATH;
}
void SetShaderSourcePath(const std::string& path)
{
	SHADERSOURCEPATH = path;
}
void ReloadShaders()
{
	device->ClearPipelineStateCache();
	SHADER_ERRORS.store(0);
	SHADER_MISSING.store(0);

	wi::eventhandler::FireEvent(wi::eventhandler::EVENT_RELOAD_SHADERS, 0);
}

void Initialize()
{
	wi::Timer timer;

	SetUpStates();
	LoadBuffers();

	static wi::eventhandler::Handle handle2 = wi::eventhandler::Subscribe(wi::eventhandler::EVENT_RELOAD_SHADERS, [](uint64_t userdata) { LoadShaders(); });
	LoadShaders();

	wi::backlog::post("wi::renderer Initialized (" + std::to_string((int)std::round(timer.elapsed())) + " ms)");
}
void ClearWorld(Scene& scene)
{
	scene.Clear();

	deferredMIPGenLock.lock();
	deferredMIPGens.clear();
	deferredMIPGenLock.unlock();

}

static const uint32_t CASCADE_COUNT = 3;
// Don't store this structure on heap!
struct SHCAM
{
	XMMATRIX view_projection;
	Frustum frustum;					// This frustum can be used for intersection test with wiPrimitive primitives
	BoundingFrustum boundingfrustum;	// This boundingfrustum can be used for frustum vs frustum intersection test

	SHCAM() = default;
	SHCAM(const XMFLOAT3& eyePos, const XMFLOAT4& rotation, float nearPlane, float farPlane, float fov) 
	{
		const XMVECTOR E = XMLoadFloat3(&eyePos);
		const XMVECTOR Q = XMQuaternionNormalize(XMLoadFloat4(&rotation));
		const XMMATRIX rot = XMMatrixRotationQuaternion(Q);
		const XMVECTOR to = XMVector3TransformNormal(XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f), rot);
		const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rot);
		const XMMATRIX V = XMMatrixLookToLH(E, to, up);
		const XMMATRIX P = XMMatrixPerspectiveFovLH(fov, 1, farPlane, nearPlane);
		view_projection = XMMatrixMultiply(V, P);
		frustum.Create(view_projection);
		
		BoundingFrustum::CreateFromMatrix(boundingfrustum, P);
		std::swap(boundingfrustum.Near, boundingfrustum.Far);
		boundingfrustum.Transform(boundingfrustum, XMMatrixInverse(nullptr, V));
		XMStoreFloat4(&boundingfrustum.Orientation, XMQuaternionNormalize(XMLoadFloat4(&boundingfrustum.Orientation)));
	};
};
inline void CreateSpotLightShadowCam(const LightComponent& light, SHCAM& shcam)
{
	shcam = SHCAM(light.position, light.rotation, 0.1f, light.GetRange(), light.outerConeAngle * 2);
}
inline void CreateDirLightShadowCams(const LightComponent& light, CameraComponent camera, SHCAM* shcams, size_t shcam_count)
{
	if (GetTemporalAAEnabled())
	{
		// remove camera jittering
		camera.jitter = XMFLOAT2(0, 0);
		camera.UpdateCamera();
	}

	const XMMATRIX lightRotation = XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation));
	const XMVECTOR to = XMVector3TransformNormal(XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f), lightRotation);
	const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), lightRotation);
	const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), to, up); // important to not move (zero out eye vector) the light view matrix itself because texel snapping must be done on projection matrix!
	const float nearPlane = camera.zNearP;
	const float farPlane = camera.zFarP;
	const float referenceFarPlane = 800.0f; // cascade splits here were tested with this depth range
	const float referenceSplitClamp = std::min(1.0f, referenceFarPlane / farPlane); // if far plane is greater than reference, do not increase cascade sizes further
	const float splits[CASCADE_COUNT + 1] = {
		referenceSplitClamp * 0.0f,		// near plane
		referenceSplitClamp * 0.01f,	// near-mid split
		referenceSplitClamp * 0.1f,		// mid-far split
		referenceSplitClamp * 1.0f,		// far plane
	};
	assert(shcam_count <= CASCADE_COUNT);

	// Unproject main frustum corners into world space (notice the reversed Z projection!):
	const XMMATRIX unproj = camera.GetInvViewProjection();
	const XMVECTOR frustum_corners[] =
	{
		XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), unproj),	// far
	};

	// Compute shadow cameras:
	for (int cascade = 0; cascade < shcam_count; ++cascade)
	{
		// Compute cascade sub-frustum in light-view-space from the main frustum corners:
		const float split_near = splits[cascade];
		const float split_far = splits[cascade + 1];
		const XMVECTOR corners[] =
		{
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far), lightView),
		};

		// Compute cascade bounding sphere center:
		XMVECTOR center = XMVectorZero();
		for (int j = 0; j < arraysize(corners); ++j)
		{
			center = XMVectorAdd(center, corners[j]);
		}
		center = center / float(arraysize(corners));

		// Compute cascade bounding sphere radius:
		float radius = 0;
		for (int j = 0; j < arraysize(corners); ++j)
		{
			radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[j], center))));
		}

		// Fit AABB onto bounding sphere:
		XMVECTOR vRadius = XMVectorReplicate(radius);
		XMVECTOR vMin = XMVectorSubtract(center, vRadius);
		XMVECTOR vMax = XMVectorAdd(center, vRadius);

		// Snap cascade to texel grid:
		const XMVECTOR extent = XMVectorSubtract(vMax, vMin);
		const XMVECTOR texelSize = extent / float(light.shadow_rect.w);
		vMin = XMVectorFloor(vMin / texelSize) * texelSize;
		vMax = XMVectorFloor(vMax / texelSize) * texelSize;
		center = (vMin + vMax) * 0.5f;

		XMFLOAT3 _center;
		XMFLOAT3 _min;
		XMFLOAT3 _max;
		XMStoreFloat3(&_center, center);
		XMStoreFloat3(&_min, vMin);
		XMStoreFloat3(&_max, vMax);

		// Extrude bounds to avoid early shadow clipping:
		float ext = abs(_center.z - _min.z);
		ext = std::max(ext, std::min(1500.0f, farPlane) * 0.5f);
		_min.z = _center.z - ext;
		_max.z = _center.z + ext;

		const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _max.z, _min.z); // notice reversed Z!

		shcams[cascade].view_projection = XMMatrixMultiply(lightView, lightProjection);
		shcams[cascade].frustum.Create(shcams[cascade].view_projection);
	}

}
inline void CreateCubemapCameras(const XMFLOAT3& position, float zNearP, float zFarP, SHCAM* shcams, size_t shcam_count)
{
	assert(shcam_count == 6);
	shcams[0] = SHCAM(position, XMFLOAT4(0.5f, -0.5f, -0.5f, -0.5f), zNearP, zFarP, XM_PIDIV2); //+x
	shcams[1] = SHCAM(position, XMFLOAT4(0.5f, 0.5f, 0.5f, -0.5f), zNearP, zFarP, XM_PIDIV2); //-x
	shcams[2] = SHCAM(position, XMFLOAT4(1, 0, 0, -0), zNearP, zFarP, XM_PIDIV2); //+y
	shcams[3] = SHCAM(position, XMFLOAT4(0, 0, 0, -1), zNearP, zFarP, XM_PIDIV2); //-y
	shcams[4] = SHCAM(position, XMFLOAT4(0.707f, 0, 0, -0.707f), zNearP, zFarP, XM_PIDIV2); //+z
	shcams[5] = SHCAM(position, XMFLOAT4(0, 0.707f, 0.707f, 0), zNearP, zFarP, XM_PIDIV2); //-z
}

ForwardEntityMaskCB ForwardEntityCullingCPU(const Visibility& vis, const AABB& batch_aabb, RENDERPASS renderPass)
{
	// Performs CPU light culling for a renderable batch:
	//	Similar to GPU-based tiled light culling, but this is only for simple forward passes (drawcall-granularity)

	ForwardEntityMaskCB cb;
	cb.xForwardLightMask.x = 0;
	cb.xForwardLightMask.y = 0;
	cb.xForwardDecalMask = 0;
	cb.xForwardEnvProbeMask = 0;

	uint32_t buckets[2] = { 0,0 };
	for (size_t i = 0; i < std::min(size_t(64), vis.visibleLights.size()); ++i) // only support indexing 64 lights at max for now
	{
		const uint32_t lightIndex = vis.visibleLights[i];
		const AABB& light_aabb = vis.scene->aabb_lights[lightIndex];
		if (light_aabb.intersects(batch_aabb))
		{
			const uint8_t bucket_index = uint8_t(i / 32);
			const uint8_t bucket_place = uint8_t(i % 32);
			buckets[bucket_index] |= 1 << bucket_place;
		}
	}
	cb.xForwardLightMask.x = buckets[0];
	cb.xForwardLightMask.y = buckets[1];

	for (size_t i = 0; i < std::min(size_t(32), vis.visibleDecals.size()); ++i)
	{
		const uint32_t decalIndex = vis.visibleDecals[vis.visibleDecals.size() - 1 - i]; // note: reverse order, for correct blending!
		const AABB& decal_aabb = vis.scene->aabb_decals[decalIndex];
		if (decal_aabb.intersects(batch_aabb))
		{
			const uint8_t bucket_place = uint8_t(i % 32);
			cb.xForwardDecalMask |= 1 << bucket_place;
		}
	}

	if (renderPass != RENDERPASS_ENVMAPCAPTURE)
	{
		for (size_t i = 0; i < std::min(size_t(32), vis.visibleEnvProbes.size()); ++i)
		{
			const uint32_t probeIndex = vis.visibleEnvProbes[vis.visibleEnvProbes.size() - 1 - i]; // note: reverse order, for correct blending!
			const AABB& probe_aabb = vis.scene->aabb_probes[probeIndex];
			if (probe_aabb.intersects(batch_aabb))
			{
				const uint8_t bucket_place = uint8_t(i % 32);
				cb.xForwardEnvProbeMask |= 1 << bucket_place;
			}
		}
	}

	return cb;
}

void Workaround(const int bug , CommandList cmd)
{
	if (bug == 1)
	{
		//PE: Strange DX12 bug, we must change the pso/pipeline state, just one time.
		//PE: After this there will be no "black dots" or culling/depth errors.
		//PE: This bug only happen on some nvidia cards ?
		//PE: https://github.com/turanszkij/WickedEngine/issues/450#issuecomment-1143647323

		//PE: We MUST use RENDERPASS_VOXELIZE (DSSTYPE_DEPTHDISABLED) or it will not work ?
		const PipelineState* pso = &PSO_object[0][RENDERPASS_VOXELIZE][BLENDMODE_OPAQUE][0][0][0];

		device->EventBegin("Workaround 1", cmd);
		static RenderPass renderpass_clear;
		if (!renderpass_clear.IsValid())
		{
			RenderPassDesc renderpassdesc;
			renderpassdesc.flags = RenderPassDesc::Flags::EMPTY;
			device->CreateRenderPass(&renderpassdesc, &renderpass_clear);
		}
		device->RenderPassBegin(&renderpass_clear, cmd);
		device->BindPipelineState(pso, cmd);
		device->DrawIndexedInstanced(0, 0, 0, 0, 0, cmd); //PE: Just need predraw(cmd);
		device->RenderPassEnd(cmd);
		device->EventEnd(cmd);
	}
	return;
}

void RenderMeshes(
	const Visibility& vis,
	const RenderQueue& renderQueue,
	RENDERPASS renderPass,
	uint32_t renderTypeFlags,
	CommandList cmd,
	bool tessellation = false,
	const Frustum* frusta = nullptr,
	uint32_t frustum_count = 1
)
{
	if (renderQueue.empty())
		return;

	device->EventBegin("RenderMeshes", cmd);

	tessellation = tessellation && device->CheckCapability(GraphicsDeviceCapability::TESSELLATION);
	
	// Do we need to compute a light mask for this pass on the CPU?
	const bool forwardLightmaskRequest =
		renderPass == RENDERPASS_ENVMAPCAPTURE ||
		renderPass == RENDERPASS_VOXELIZE;

	// Pre-allocate space for all the instances in GPU-buffer:
	const size_t alloc_size = renderQueue.size() * frustum_count * sizeof(ShaderMeshInstancePointer);
	const GraphicsDevice::GPUAllocation instances = device->AllocateGPU(alloc_size, cmd);
	const int instanceBufferDescriptorIndex = device->GetDescriptorIndex(&instances.buffer, SubresourceType::SRV);

	// This will correspond to a single draw call
	//	It's used to render multiple instances of a single mesh
	struct InstancedBatch
	{
		uint32_t meshIndex = ~0u;
		uint32_t instanceCount = 0;
		uint32_t dataOffset = 0;
		uint8_t userStencilRefOverride = 0;
		bool forceAlphatestForDithering = false;
		AABB aabb;
		uint32_t lod = 0;
	} instancedBatch = {};


	// This will be called every time we start a new draw call:
	auto batch_flush = [&]()
	{
		if (instancedBatch.instanceCount == 0)
			return;
		const MeshComponent& mesh = vis.scene->meshes[instancedBatch.meshIndex];
		const bool forceAlphaTestForDithering = instancedBatch.forceAlphatestForDithering != 0;
		const uint8_t userStencilRefOverride = instancedBatch.userStencilRefOverride;

		const float tessF = mesh.GetTessellationFactor();
		const bool tessellatorRequested = tessF > 0 && tessellation;

		if (forwardLightmaskRequest)
		{
			ForwardEntityMaskCB cb = ForwardEntityCullingCPU(vis, instancedBatch.aabb, renderPass);
			device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(ForwardEntityMaskCB), cmd);
		}

		device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

		uint32_t first_subset = 0;
		uint32_t last_subset = 0;
		mesh.GetLODSubsetRange(instancedBatch.lod, first_subset, last_subset);
		for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
		{
			const MeshComponent::MeshSubset& subset = mesh.subsets[subsetIndex];
			if (subset.indexCount == 0)
			{
				continue;
			}
			const MaterialComponent& material = vis.scene->materials[subset.materialIndex];

			bool subsetRenderable = renderTypeFlags & material.GetRenderTypes();

			if (renderPass == RENDERPASS_SHADOW || renderPass == RENDERPASS_SHADOWCUBE)
			{
				subsetRenderable = subsetRenderable && material.IsCastingShadow();
			}

			if (!subsetRenderable)
			{
				continue;
			}

			const PipelineState* pso = nullptr;
			const PipelineState* pso_backside = nullptr; // only when separate backside rendering is required (transparent doublesided)
			{
				if (IsWireRender())
				{
					switch (renderPass)
					{
					case RENDERPASS_MAIN:
						pso = tessellatorRequested ? &PSO_object_wire_tessellation : &PSO_object_wire;
					}
				}
				else if (material.customShaderID >= 0 && material.customShaderID < (int)customShaders.size())
				{
					const CustomShader& customShader = customShaders[material.customShaderID];
					if (renderTypeFlags & customShader.renderTypeFlags)
					{
						pso = &customShader.pso[renderPass];
					}
				}
				else
				{
					const BLENDMODE blendMode = material.GetBlendMode();
					const bool alphatest = material.IsAlphaTestEnabled() || forceAlphaTestForDithering;
					OBJECTRENDERING_DOUBLESIDED doublesided = (mesh.IsDoubleSided() || material.IsDoubleSided()) ? OBJECTRENDERING_DOUBLESIDED_ENABLED : OBJECTRENDERING_DOUBLESIDED_DISABLED;

					pso = &PSO_object[material.shaderType][renderPass][blendMode][doublesided][tessellatorRequested][alphatest];
					assert(pso->IsValid());

					if ((renderTypeFlags & RENDERTYPE_TRANSPARENT) && doublesided == OBJECTRENDERING_DOUBLESIDED_ENABLED)
					{
						doublesided = OBJECTRENDERING_DOUBLESIDED_BACKSIDE;
						pso_backside = &PSO_object[material.shaderType][renderPass][blendMode][doublesided][tessellatorRequested][alphatest];
					}
				}
			}

			if (pso == nullptr || !pso->IsValid())
			{
				continue;
			}

			STENCILREF engineStencilRef = material.engineStencilRef;
			uint8_t userStencilRef = userStencilRefOverride > 0 ? userStencilRefOverride : material.userStencilRef;
			uint32_t stencilRef = CombineStencilrefs(engineStencilRef, userStencilRef);
			device->BindStencilRef(stencilRef, cmd);

			if (renderPass != RENDERPASS_PREPASS && renderPass != RENDERPASS_VOXELIZE) // depth only alpha test will be full res
			{
				device->BindShadingRate(material.shadingRate, cmd);
			}

			assert(subsetIndex < 256u); // subsets must be represented as 8-bit

			ObjectPushConstants push;
			push.geometryIndex = mesh.geometryOffset + subsetIndex;
			push.materialIndex = subset.materialIndex;
			push.instances = instanceBufferDescriptorIndex;
			push.instance_offset = (uint)instancedBatch.dataOffset;

			if (pso_backside != nullptr)
			{
				device->BindPipelineState(pso_backside, cmd);
				device->PushConstants(&push, sizeof(push), cmd);
				device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, subset.indexOffset, 0, 0, cmd);
			}

			device->BindPipelineState(pso, cmd);
			device->PushConstants(&push, sizeof(push), cmd);
			device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, subset.indexOffset, 0, 0, cmd);

		}
	};

	// The following loop is writing the instancing batches to a GPUBuffer:
	//	RenderQueue is sorted based on mesh index, so when a new mesh or stencil request is encountered, we need to flush the batch
	uint32_t instanceCount = 0;
	for (const RenderBatch& batch : renderQueue.batches) // Do not break out of this loop!
	{
		const uint32_t meshIndex = batch.GetMeshIndex();
		const uint32_t instanceIndex = batch.GetInstanceIndex();
		const ObjectComponent& instance = vis.scene->objects[instanceIndex];
		const AABB& instanceAABB = vis.scene->aabb_objects[instanceIndex];
		const uint8_t userStencilRefOverride = instance.userStencilRef;

		// When we encounter a new mesh inside the global instance array, we begin a new RenderBatch:
		if (meshIndex != instancedBatch.meshIndex ||
			userStencilRefOverride != instancedBatch.userStencilRefOverride ||
			instance.lod != instancedBatch.lod
			)
		{
			batch_flush();

			instancedBatch = {};
			instancedBatch.meshIndex = meshIndex;
			instancedBatch.instanceCount = 0;
			instancedBatch.dataOffset = (uint32_t)(instances.offset + instanceCount * sizeof(ShaderMeshInstancePointer));
			instancedBatch.userStencilRefOverride = userStencilRefOverride;
			instancedBatch.forceAlphatestForDithering = 0;
			instancedBatch.aabb = AABB();
			instancedBatch.lod = instance.lod;
		}

		const float dither = std::max(instance.GetTransparency(), std::max(0.0f, batch.GetDistance() - instance.fadeDistance) / instance.radius);
		if (dither > 0)
		{
			instancedBatch.forceAlphatestForDithering = 1;
		}

		if (forwardLightmaskRequest)
		{
			instancedBatch.aabb = AABB::Merge(instancedBatch.aabb, instanceAABB);
		}

		for (uint32_t frustum_index = 0; frustum_index < frustum_count; ++frustum_index)
		{
			if (frusta != nullptr && !frusta[frustum_index].CheckBoxFast(instanceAABB))
			{
				// In case multiple cameras were provided and no intersection detected with frustum, we don't add the instance for the face:
				continue;
			}

			ShaderMeshInstancePointer poi;
			poi.Create(instanceIndex, frustum_index, dither);

			// Write into actual GPU-buffer:
			std::memcpy((ShaderMeshInstancePointer*)instances.data + instanceCount, &poi, sizeof(poi)); // memcpy whole structure into mapped pointer to avoid read from uncached memory

			instancedBatch.instanceCount++; // next instance in current InstancedBatch
			instanceCount++;
		}

	}

	batch_flush();

	device->EventEnd(cmd);
}

void RenderImpostors(
	const Visibility& vis,
	RENDERPASS renderPass, 
	CommandList cmd
)
{
	const PipelineState* pso = &PSO_impostor[renderPass];
	if (IsWireRender())
	{
		if (renderPass != RENDERPASS_PREPASS)
		{
			pso = &PSO_impostor_wire;
		}
		else
		{
			return;
		}
	}

	if (vis.scene->impostors.GetCount() > 0 && pso != nullptr && vis.scene->impostorBuffer.IsValid())
	{
		device->EventBegin("RenderImpostors", cmd);

		device->BindStencilRef(STENCILREF_DEFAULT, cmd);
		device->BindPipelineState(pso, cmd);

		device->BindIndexBuffer(
			&vis.scene->impostorBuffer,
			vis.scene->impostor_ib_format == Format::R32_UINT ? IndexBufferFormat::UINT32 : IndexBufferFormat::UINT16,
			vis.scene->impostor_ib.offset,
			cmd
		);
		device->BindResource(&vis.scene->impostorBuffer, 0, cmd, vis.scene->impostor_vb.subresource_srv);
		device->BindResource(&vis.scene->impostorBuffer, 2, cmd, vis.scene->impostor_data.subresource_srv);
		device->BindResource(&vis.scene->impostorArray, 1, cmd);

		device->DrawIndexedInstancedIndirect(&vis.scene->impostorIndirectBuffer, 0, cmd);

		device->EventEnd(cmd);
	}
}

void ProcessDeferredMipGenRequests(CommandList cmd)
{
	deferredMIPGenLock.lock();
	for (auto& it : deferredMIPGens)
	{
		MIPGEN_OPTIONS mipopt;
		mipopt.preserve_coverage = it.second;
		GenerateMipChain(it.first, MIPGENFILTER_LINEAR, cmd, mipopt);
	}
	deferredMIPGens.clear();
	deferredMIPGenLock.unlock();
}

void UpdateVisibility(Visibility& vis)
{
	// Perform parallel frustum culling and obtain closest reflector:
	wi::jobsystem::context ctx;
	auto range = wi::profiler::BeginRangeCPU("Frustum Culling");

	assert(vis.scene != nullptr); // User must provide a scene!
	assert(vis.camera != nullptr); // User must provide a camera!

	// The parallel frustum culling is first performed in shared memory, 
	//	then each group writes out it's local list to global memory
	//	The shared memory approach reduces atomics and helps the list to remain
	//	more coherent (less randomly organized compared to original order)
	static const uint32_t groupSize = 64;
	static const size_t sharedmemory_size = (groupSize + 1) * sizeof(uint32_t); // list + counter per group

	// Initialize visible indices:
	vis.Clear();

	if (!GetFreezeCullingCameraEnabled())
	{
		vis.frustum = vis.camera->frustum;
	}

	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled())
	{
		vis.flags &= ~Visibility::ALLOW_OCCLUSION_CULLING;
	}

	if (vis.flags & Visibility::ALLOW_LIGHTS)
	{
		// Cull lights:
		vis.visibleLights.resize(vis.scene->aabb_lights.GetCount());
		wi::jobsystem::Dispatch(ctx, (uint32_t)vis.scene->aabb_lights.GetCount(), groupSize, [&](wi::jobsystem::JobArgs args) {

			// Setup stream compaction:
			uint32_t& group_count = *(uint32_t*)args.sharedmemory;
			uint32_t* group_list = (uint32_t*)args.sharedmemory + 1;
			if (args.isFirstJobInGroup)
			{
				group_count = 0; // first thread initializes local counter
			}

			const AABB& aabb = vis.scene->aabb_lights[args.jobIndex];

			if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
			{
				// Local stream compaction:
				//	(also compute light distance for shadow priority sorting)
				group_list[group_count] = args.jobIndex;
				const LightComponent& light = vis.scene->lights[args.jobIndex];
				group_count++;
				if (light.IsVolumetricsEnabled())
				{
					vis.volumetriclight_request.store(true);
				}

				if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
				{
					if (!light.IsStatic() && light.GetType() != LightComponent::DIRECTIONAL || light.occlusionquery < 0)
					{
						if (!aabb.intersects(vis.camera->Eye))
						{
							light.occlusionquery = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
						}
					}
				}
			}

			// Global stream compaction:
			if (args.isLastJobInGroup && group_count > 0)
			{
				uint32_t prev_count = vis.light_counter.fetch_add(group_count);
				for (uint32_t i = 0; i < group_count; ++i)
				{
					vis.visibleLights[prev_count + i] = group_list[i];
				}
			}

			}, sharedmemory_size);
	}

	if (vis.flags & Visibility::ALLOW_OBJECTS)
	{
		// Cull objects:
		vis.visibleObjects.resize(vis.scene->aabb_objects.GetCount());
		wi::jobsystem::Dispatch(ctx, (uint32_t)vis.scene->aabb_objects.GetCount(), groupSize, [&](wi::jobsystem::JobArgs args) {

			// Setup stream compaction:
			uint32_t& group_count = *(uint32_t*)args.sharedmemory;
			uint32_t* group_list = (uint32_t*)args.sharedmemory + 1;
			if (args.isFirstJobInGroup)
			{
				group_count = 0; // first thread initializes local counter
			}

			const AABB& aabb = vis.scene->aabb_objects[args.jobIndex];

			if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
			{
				// Local stream compaction:
				group_list[group_count++] = args.jobIndex;

				const ObjectComponent& object = vis.scene->objects[args.jobIndex];

				if (vis.flags & Visibility::ALLOW_REQUEST_REFLECTION)
				{
					if (object.IsRequestPlanarReflection())
					{
						float dist = wi::math::DistanceEstimated(vis.camera->Eye, object.center);
						vis.locker.lock();
						if (dist < vis.closestRefPlane)
						{
							vis.closestRefPlane = dist;
							XMVECTOR P = XMLoadFloat3(&object.center);
							XMVECTOR N = XMVectorSet(0, 1, 0, 0);
							N = XMVector3TransformNormal(N, XMLoadFloat4x4(&object.worldMatrix));
							XMVECTOR _refPlane = XMPlaneFromPointNormal(P, N);
							XMStoreFloat4(&vis.reflectionPlane, _refPlane);

							vis.planar_reflection_visible = true;
						}
						vis.locker.unlock();
					}
				}

				if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
				{
					if (object.IsRenderable() && object.occlusionQueries[vis.scene->queryheap_idx] < 0)
					{
						if (aabb.intersects(vis.camera->Eye))
						{
							// camera is inside the instance, mark it as visible in this frame:
							object.occlusionHistory |= 1;
						}
						else
						{
							object.occlusionQueries[vis.scene->queryheap_idx] = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
						}
					}
				}
			}

			// Global stream compaction:
			if (args.isLastJobInGroup && group_count > 0)
			{
				uint32_t prev_count = vis.object_counter.fetch_add(group_count);
				for (uint32_t i = 0; i < group_count; ++i)
				{
					vis.visibleObjects[prev_count + i] = group_list[i];
				}
			}

			}, sharedmemory_size);
	}

	if (vis.flags & Visibility::ALLOW_DECALS)
	{
		vis.visibleDecals.resize(vis.scene->aabb_decals.GetCount());
		wi::jobsystem::Dispatch(ctx, (uint32_t)vis.scene->aabb_decals.GetCount(), groupSize, [&](wi::jobsystem::JobArgs args) {

			// Setup stream compaction:
			uint32_t& group_count = *(uint32_t*)args.sharedmemory;
			uint32_t* group_list = (uint32_t*)args.sharedmemory + 1;
			if (args.isFirstJobInGroup)
			{
				group_count = 0; // first thread initializes local counter
			}

			const AABB& aabb = vis.scene->aabb_decals[args.jobIndex];

			if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
			{
				// Local stream compaction:
				group_list[group_count++] = args.jobIndex;
			}

			// Global stream compaction:
			if (args.isLastJobInGroup && group_count > 0)
			{
				uint32_t prev_count = vis.decal_counter.fetch_add(group_count);
				for (uint32_t i = 0; i < group_count; ++i)
				{
					vis.visibleDecals[prev_count + i] = group_list[i];
				}
			}

			}, sharedmemory_size);
	}

	if (vis.flags & Visibility::ALLOW_ENVPROBES)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			// Cull probes:
			for (size_t i = 0; i < vis.scene->aabb_probes.GetCount(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_probes[i];

				if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
				{
					vis.visibleEnvProbes.push_back((uint32_t)i);
				}
			}
			});
	}

	if (vis.flags & Visibility::ALLOW_EMITTERS)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			// Cull emitters:
			for (size_t i = 0; i < vis.scene->emitters.GetCount(); ++i)
			{
				const wi::EmittedParticleSystem& emitter = vis.scene->emitters[i];
				if (!(emitter.layerMask & vis.layerMask))
				{
					continue;
				}
				vis.visibleEmitters.push_back((uint32_t)i);
			}
			});
	}

	if (vis.flags & Visibility::ALLOW_HAIRS)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			// Cull hairs:
			for (size_t i = 0; i < vis.scene->hairs.GetCount(); ++i)
			{
				const wi::HairParticleSystem& hair = vis.scene->hairs[i];
				if (!(hair.layerMask & vis.layerMask))
				{
					continue;
				}
				if (!hair.regenerate_frame)
				{
					const float dist = wi::math::Distance(vis.camera->Eye, hair.aabb.getCenter());
					const float radius = hair.aabb.getRadius();
					if (dist - radius > hair.viewDistance)
						continue;
				}
				if (hair.meshID == INVALID_ENTITY || !vis.frustum.CheckBoxFast(hair.aabb))
				{
					continue;
				}
				vis.visibleHairs.push_back((uint32_t)i);
			}
			});
	}

	wi::jobsystem::Wait(ctx);

	// finalize stream compaction:
	vis.visibleObjects.resize((size_t)vis.object_counter.load());
	vis.visibleDecals.resize((size_t)vis.decal_counter.load());
	vis.visibleLights.resize((size_t)vis.light_counter.load());

	if (vis.scene->weather.IsOceanEnabled())
	{
		bool occluded = false;
		if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
		{
			vis.scene->ocean.occlusionQueries[vis.scene->queryheap_idx] = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
			if (vis.scene->ocean.IsOccluded())
			{
				occluded = true;
			}
		}

		if ((vis.flags & Visibility::ALLOW_REQUEST_REFLECTION) && !occluded)
		{
			// Ocean will override any current reflectors
			vis.planar_reflection_visible = true;
			XMVECTOR _refPlane = XMPlaneFromPointNormal(XMVectorSet(0, vis.scene->weather.oceanParameters.waterHeight, 0, 0), XMVectorSet(0, 1, 0, 0));
			XMStoreFloat4(&vis.reflectionPlane, _refPlane);
		}
	}

	wi::profiler::EndRange(range); // Frustum Culling
}
void UpdatePerFrameData(
	Scene& scene,
	const Visibility& vis,
	FrameCB& frameCB,
	float dt
)
{
	// Update Voxelization parameters:
	if (scene.objects.GetCount() > 0)
	{
		// We don't update it if the scene is empty, this even makes it easier to debug
		const float f = 0.05f / voxelSceneData.voxelsize;
		XMFLOAT3 center = XMFLOAT3(std::floor(vis.camera->Eye.x * f) / f, std::floor(vis.camera->Eye.y * f) / f, std::floor(vis.camera->Eye.z * f) / f);
		if (wi::math::DistanceSquared(center, voxelSceneData.center) > 0)
		{
			voxelSceneData.centerChangedThisFrame = true;
		}
		else
		{
			voxelSceneData.centerChangedThisFrame = false;
		}
		voxelSceneData.center = center;
		voxelSceneData.extents = XMFLOAT3(voxelSceneData.res * voxelSceneData.voxelsize, voxelSceneData.res * voxelSceneData.voxelsize, voxelSceneData.res * voxelSceneData.voxelsize);
	}

	// Shadow atlas packing:
	if (!vis.visibleLights.empty())
	{
		auto range = wi::profiler::BeginRangeCPU("Shadowmap packing");
		static thread_local wi::rectpacker::State packer;
		float iterative_scaling = 1;

		while (iterative_scaling > 0.03f)
		{
			packer.clear();
			for (uint32_t lightIndex : vis.visibleLights)
			{
				LightComponent& light = scene.lights[lightIndex];
				light.shadow_rect = {};
				if (!light.IsCastingShadow() || light.IsStatic())
					continue;

				const float dist = wi::math::Distance(vis.camera->Eye, light.position);
				const float range = light.GetRange();
				const float amount = std::min(1.0f, range / std::max(0.001f, dist)) * iterative_scaling;

				wi::rectpacker::Rect rect = {};
				rect.id = int(lightIndex);
				switch (light.GetType())
				{
				case LightComponent::DIRECTIONAL:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = light.forced_shadow_resolution * int(CASCADE_COUNT);
						rect.h = light.forced_shadow_resolution;
					}
					else
					{
						rect.w = int(max_shadow_resolution_2D * iterative_scaling) * int(CASCADE_COUNT);
						rect.h = int(max_shadow_resolution_2D * iterative_scaling);
					}
					break;
				case LightComponent::SPOT:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = int(light.forced_shadow_resolution);
						rect.h = int(light.forced_shadow_resolution);
					}
					else
					{
						rect.w = int(max_shadow_resolution_2D * amount);
						rect.h = int(max_shadow_resolution_2D * amount);
					}
					break;
				case LightComponent::POINT:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = int(light.forced_shadow_resolution) * 6;
						rect.h = int(light.forced_shadow_resolution);
					}
					else
					{
						rect.w = int(max_shadow_resolution_cube * amount) * 6;
						rect.h = int(max_shadow_resolution_cube * amount);
					}
					break;
				}
				if (rect.w > 8 && rect.h > 8)
				{
					packer.add_rect(rect);
				}
			}
			if (!packer.rects.empty())
			{
				if (packer.pack(8192))
				{
					for (auto& rect : packer.rects)
					{
						uint32_t lightIndex = uint32_t(rect.id);
						LightComponent& light = scene.lights[lightIndex];
						if (rect.was_packed)
						{
							light.shadow_rect = rect;

							// Remove slice multipliers from rect:
							switch (light.GetType())
							{
							case LightComponent::DIRECTIONAL:
								light.shadow_rect.w /= int(CASCADE_COUNT);
								break;
							case LightComponent::POINT:
								light.shadow_rect.w /= 6;
								break;
							}
						}
						else
						{
							light.direction = {};
						}
					}

					if ((int)shadowMapAtlas.desc.width < packer.width || (int)shadowMapAtlas.desc.height < packer.height)
					{
						TextureDesc desc;
						desc.width = uint32_t(packer.width);
						desc.height = uint32_t(packer.height);
						desc.format = Format::R16_TYPELESS;
						desc.bind_flags = BindFlag::DEPTH_STENCIL | BindFlag::SHADER_RESOURCE;
						desc.layout = ResourceState::SHADER_RESOURCE;
						device->CreateTexture(&desc, nullptr, &shadowMapAtlas);
						device->SetName(&shadowMapAtlas, "shadowMapAtlas");

						desc.format = Format::R16G16B16A16_FLOAT;
						desc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE;
						desc.layout = ResourceState::SHADER_RESOURCE;
						desc.clear.color[0] = 1;
						desc.clear.color[1] = 1;
						desc.clear.color[2] = 1;
						desc.clear.color[3] = 0;
						device->CreateTexture(&desc, nullptr, &shadowMapAtlas_Transparent);
						device->SetName(&shadowMapAtlas_Transparent, "shadowMapAtlas_Transparent");


						RenderPassDesc renderpassdesc;
						renderpassdesc.attachments.push_back(
							RenderPassAttachment::DepthStencil(
								&shadowMapAtlas,
								RenderPassAttachment::LoadOp::CLEAR,
								RenderPassAttachment::StoreOp::STORE,
								ResourceState::SHADER_RESOURCE,
								ResourceState::DEPTHSTENCIL,
								ResourceState::SHADER_RESOURCE
							)
						);
						renderpassdesc.attachments.push_back(
							RenderPassAttachment::RenderTarget(
								&shadowMapAtlas_Transparent,
								RenderPassAttachment::LoadOp::CLEAR,
								RenderPassAttachment::StoreOp::STORE,
								ResourceState::SHADER_RESOURCE,
								ResourceState::RENDERTARGET,
								ResourceState::SHADER_RESOURCE
							)
						);
						device->CreateRenderPass(&renderpassdesc, &renderpass_shadowMapAtlas);
					}

					break;
				}
				else
				{
					iterative_scaling *= 0.5f;
				}
			}
			else
			{
				iterative_scaling = 0.0; //PE: fix - endless loop if some lights do not have shadows.
			}
		}
		wi::profiler::EndRange(range);
	}

	// Update CPU-side frame constant buffer:
	frameCB.delta_time = dt * GetGameSpeed();
	frameCB.time_previous = frameCB.time;
	frameCB.time += frameCB.delta_time;
	frameCB.frame_count = (uint)device->GetFrameCount();
	frameCB.blue_noise_phase = (frameCB.frame_count & 0xFF) * 1.6180339887f;

	frameCB.voxelradiance_max_distance = voxelSceneData.maxDistance;
	frameCB.voxelradiance_size = voxelSceneData.voxelsize;
	frameCB.voxelradiance_size_rcp = 1.0f / (float)frameCB.voxelradiance_size;
	frameCB.voxelradiance_resolution = GetVoxelRadianceEnabled() ? (uint)voxelSceneData.res : 0;
	frameCB.voxelradiance_resolution_rcp = GetVoxelRadianceEnabled() ? 1.0f / (float)frameCB.voxelradiance_resolution : 0; //PE: was inf.
	frameCB.voxelradiance_mipcount = voxelSceneData.mips;
	frameCB.voxelradiance_numcones = std::max(std::min(voxelSceneData.numCones, 16u), 1u);
	frameCB.voxelradiance_numcones_rcp = 1.0f / (float)frameCB.voxelradiance_numcones;
	frameCB.voxelradiance_stepsize = voxelSceneData.rayStepSize;
	frameCB.voxelradiance_center = voxelSceneData.center;

	// The order is very important here:
	frameCB.decalarray_offset = 0;
	frameCB.decalarray_count = (uint)vis.visibleDecals.size();
	frameCB.envprobearray_offset = frameCB.decalarray_count;
	frameCB.envprobearray_count = std::min(vis.scene->envmapCount, (uint)vis.visibleEnvProbes.size());
	frameCB.lightarray_offset = frameCB.envprobearray_offset + frameCB.envprobearray_count;
	frameCB.lightarray_count = (uint)vis.visibleLights.size();
	frameCB.forcefieldarray_offset = frameCB.lightarray_offset + frameCB.lightarray_count;
	frameCB.forcefieldarray_count = (uint)vis.scene->forces.GetCount();

	frameCB.envprobe_mipcount = 0;
	frameCB.envprobe_mipcount_rcp = 1.0f;
	if (vis.scene->envmapArray.IsValid())
	{
		frameCB.envprobe_mipcount = vis.scene->envmapArray.GetDesc().mip_levels;
		frameCB.envprobe_mipcount_rcp = 1.0f / (float)frameCB.envprobe_mipcount;
	}

	frameCB.gi_boost = GetGIBoost();

	frameCB.temporalaa_samplerotation = 0;
	if (GetTemporalAAEnabled())
	{
		uint id = frameCB.frame_count % 4;
		uint x = 0;
		uint y = 0;
		switch (id)
		{
		case 1:
			x = 1;
			break;
		case 2:
			y = 1;
			break;
		case 3:
			x = 1;
			y = 1;
			break;
		default:
			break;
		}
		frameCB.temporalaa_samplerotation = (x & 0x000000FF) | ((y & 0x000000FF) << 8);
	}

	frameCB.options = 0;
	if (GetTemporalAAEnabled())
	{
		frameCB.options |= OPTION_BIT_TEMPORALAA_ENABLED;
	}
	if (GetTransparentShadowsEnabled())
	{
		frameCB.options |= OPTION_BIT_TRANSPARENTSHADOWS_ENABLED;
	}
	if (GetVoxelRadianceEnabled())
	{
		frameCB.options |= OPTION_BIT_VOXELGI_ENABLED;
	}
	if (GetVoxelRadianceReflectionsEnabled())
	{
		frameCB.options |= OPTION_BIT_VOXELGI_REFLECTIONS_ENABLED;
	}
	if (voxelSceneData.centerChangedThisFrame)
	{
		frameCB.options |= OPTION_BIT_VOXELGI_RETARGETTED;
	}
	if (vis.scene->weather.IsSimpleSky())
	{
		frameCB.options |= OPTION_BIT_SIMPLE_SKY;
	}
	if (vis.scene->weather.IsRealisticSky())
	{
		frameCB.options |= OPTION_BIT_REALISTIC_SKY;
	}
	if (vis.scene->weather.IsHeightFog())
	{
		frameCB.options |= OPTION_BIT_HEIGHT_FOG;
	}
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING) && GetRaytracedShadowsEnabled())
	{
		frameCB.options |= OPTION_BIT_RAYTRACED_SHADOWS;
		frameCB.options |= OPTION_BIT_SHADOW_MASK;
	}
	if (GetScreenSpaceShadowsEnabled())
	{
		frameCB.options |= OPTION_BIT_SHADOW_MASK;
	}
	if (GetSurfelGIEnabled())
	{
		frameCB.options |= OPTION_BIT_SURFELGI_ENABLED;
	}
	if (IsDisableAlbedoMaps())
	{
		frameCB.options |= OPTION_BIT_DISABLE_ALBEDO_MAPS;
	}
	if (IsForceDiffuseLighting())
	{
		frameCB.options |= OPTION_BIT_FORCE_DIFFUSE_LIGHTING;
	}
	if (vis.scene->weather.skyMap.IsValid())
	{
		bool hdr = !IsFormatUnorm(vis.scene->weather.skyMap.GetTexture().desc.format);
		if (hdr)
		{
			frameCB.options |= OPTION_BIT_STATIC_SKY_HDR;
		}
	}

	frameCB.scene = vis.scene->shaderscene;

	frameCB.sampler_objectshader_index = device->GetDescriptorIndex(&samplers[SAMPLER_OBJECTSHADER]);
	frameCB.texture_random64x64_index = device->GetDescriptorIndex(wi::texturehelper::getRandom64x64(), SubresourceType::SRV);
	frameCB.texture_bluenoise_index = device->GetDescriptorIndex(wi::texturehelper::getBlueNoise(), SubresourceType::SRV);
	frameCB.texture_sheenlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SHEENLUT], SubresourceType::SRV);
	frameCB.texture_skyviewlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], SubresourceType::SRV);
	frameCB.texture_transmittancelut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], SubresourceType::SRV);
	frameCB.texture_multiscatteringlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], SubresourceType::SRV);
	frameCB.texture_skyluminancelut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], SubresourceType::SRV);
	frameCB.texture_voxelgi_index = device->GetDescriptorIndex(GetVoxelRadianceSecondaryBounceEnabled() ? &textures[TEXTYPE_3D_VOXELRADIANCE_HELPER] : &textures[TEXTYPE_3D_VOXELRADIANCE], SubresourceType::SRV);
	frameCB.buffer_entityarray_index = device->GetDescriptorIndex(&resourceBuffers[RBTYPE_ENTITYARRAY], SubresourceType::SRV);
	frameCB.buffer_entitymatrixarray_index = device->GetDescriptorIndex(&resourceBuffers[RBTYPE_MATRIXARRAY], SubresourceType::SRV);

	frameCB.shadow_cascade_count = CASCADE_COUNT;
	frameCB.texture_shadowatlas_index = device->GetDescriptorIndex(&shadowMapAtlas, SubresourceType::SRV);
	frameCB.texture_shadowatlas_transparent_index = device->GetDescriptorIndex(&shadowMapAtlas_Transparent, SubresourceType::SRV);
	frameCB.shadow_atlas_resolution.x = shadowMapAtlas.desc.width;
	frameCB.shadow_atlas_resolution.y = shadowMapAtlas.desc.height;
	frameCB.shadow_atlas_resolution_rcp.x = 1.0f / frameCB.shadow_atlas_resolution.x;
	frameCB.shadow_atlas_resolution_rcp.y = 1.0f / frameCB.shadow_atlas_resolution.y;


	// Create volumetric cloud static resources if needed:
	if (scene.weather.IsVolumetricClouds() && !texture_shapeNoise.IsValid())
	{
		TextureDesc shape_desc;
		shape_desc.type = TextureDesc::Type::TEXTURE_3D;
		shape_desc.width = 64;
		shape_desc.height = 64;
		shape_desc.depth = 64;
		shape_desc.mip_levels = 6;
		shape_desc.format = Format::R8G8B8A8_UNORM;
		shape_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		shape_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&shape_desc, nullptr, &texture_shapeNoise);
		device->SetName(&texture_shapeNoise, "texture_shapeNoise");

		for (uint32_t i = 0; i < texture_shapeNoise.GetDesc().mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&texture_shapeNoise, SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&texture_shapeNoise, SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}

		TextureDesc detail_desc;
		detail_desc.type = TextureDesc::Type::TEXTURE_3D;
		detail_desc.width = 32;
		detail_desc.height = 32;
		detail_desc.depth = 32;
		detail_desc.mip_levels = 6;
		detail_desc.format = Format::R8G8B8A8_UNORM;
		detail_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		detail_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&detail_desc, nullptr, &texture_detailNoise);
		device->SetName(&texture_detailNoise, "texture_detailNoise");

		for (uint32_t i = 0; i < texture_detailNoise.GetDesc().mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&texture_detailNoise, SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&texture_detailNoise, SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}

		TextureDesc texture_desc;
		texture_desc.type = TextureDesc::Type::TEXTURE_2D;
		texture_desc.width = 128;
		texture_desc.height = 128;
		texture_desc.format = Format::R8G8B8A8_UNORM;
		texture_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		texture_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&texture_desc, nullptr, &texture_curlNoise);
		device->SetName(&texture_curlNoise, "texture_curlNoise");

		texture_desc.width = 1024;
		texture_desc.height = 1024;
		texture_desc.format = Format::R8G8B8A8_UNORM;
		device->CreateTexture(&texture_desc, nullptr, &texture_weatherMap);
		device->SetName(&texture_weatherMap, "texture_weatherMap");
	}
}
void UpdateRenderData(
	const Visibility& vis,
	const FrameCB& frameCB,
	CommandList cmd
)
{
	device->EventBegin("UpdateRenderData", cmd);

	auto prof_updatebuffer_cpu = wi::profiler::BeginRangeCPU("Update Buffers (CPU)");
	auto prof_updatebuffer_gpu = wi::profiler::BeginRangeGPU("Update Buffers (GPU)", cmd);

	device->UpdateBuffer(&constantBuffers[CBTYPE_FRAME], &frameCB, cmd);
	barrier_stack.push_back(GPUBarrier::Buffer(&constantBuffers[CBTYPE_FRAME], ResourceState::COPY_DST, ResourceState::CONSTANT_BUFFER));

	if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->instanceBuffer,
			0,
			&vis.scene->instanceUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->instanceArraySize * sizeof(ShaderMeshInstance),
			cmd
		);
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->instanceBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->geometryBuffer.IsValid() && vis.scene->geometryArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->geometryBuffer,
			0,
			&vis.scene->geometryUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->geometryArraySize * sizeof(ShaderGeometry),
			cmd
		);
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->geometryBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->materialBuffer.IsValid() && vis.scene->materialArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->materialBuffer,
			0,
			&vis.scene->materialUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->materialArraySize * sizeof(ShaderMaterial),
			cmd
		);
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->materialBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	// Fill Entity Array with decals + envprobes + lights in the frustum:
	{
		// Reserve temporary entity array for GPU data upload:
		auto allocation_entityarray = device->AllocateGPU(sizeof(ShaderEntity) * SHADER_ENTITY_COUNT, cmd);
		auto allocation_matrixarray = device->AllocateGPU(sizeof(XMMATRIX) * MATRIXARRAY_COUNT, cmd);
		ShaderEntity* entityArray = (ShaderEntity*)allocation_entityarray.data;
		XMMATRIX* matrixArray = (XMMATRIX*)allocation_matrixarray.data;

		const XMMATRIX viewMatrix = vis.camera->GetView();

		uint32_t entityCounter = 0;
		uint32_t matrixCounter = 0;

		// Write decals into entity array:
		for (size_t i = 0; i < vis.visibleDecals.size(); ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				assert(0); // too many decals, can't upload the rest to matrixarray!
				matrixCounter--;
				break;
			}
			ShaderEntity shaderentity = {};
			XMMATRIX shadermatrix;

			const uint32_t decalIndex = vis.visibleDecals[vis.visibleDecals.size() - 1 - i]; // note: reverse order, for correct blending!
			const DecalComponent& decal = vis.scene->decals[decalIndex];

			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->decals.GetEntity(decalIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_DECAL);
			shaderentity.position = decal.position;
			shaderentity.SetRange(decal.range);
			float emissive_mul = 1 + decal.emissive;
			shaderentity.SetColor(float4(decal.color.x * emissive_mul, decal.color.y * emissive_mul, decal.color.z * emissive_mul, decal.color.w));

			shaderentity.SetIndices(matrixCounter, 0);
			shadermatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&decal.world));

			int texture = -1;
			if (decal.texture.IsValid())
			{
				texture = device->GetDescriptorIndex(&decal.texture.GetTexture(), SubresourceType::SRV);
			}
			int normal = -1;
			if (decal.normal.IsValid())
			{
				normal = device->GetDescriptorIndex(&decal.normal.GetTexture(), SubresourceType::SRV);
			}
			shadermatrix.r[0] = XMVectorSetW(shadermatrix.r[0], *(float*)&texture);
			shadermatrix.r[1] = XMVectorSetW(shadermatrix.r[1], *(float*)&normal);

			std::memcpy(matrixArray + matrixCounter, &shadermatrix, sizeof(XMMATRIX));
			matrixCounter++;

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
		}

		// Write environment probes into entity array:
		for (size_t i = 0; i < std::min((size_t)vis.scene->envmapCount, vis.visibleEnvProbes.size()); ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				assert(0); // too many probes, can't upload the rest to matrixarray!
				matrixCounter--;
				break;
			}
			ShaderEntity shaderentity = {};
			XMMATRIX shadermatrix;

			const uint32_t probeIndex = vis.visibleEnvProbes[vis.visibleEnvProbes.size() - 1 - i]; // note: reverse order, for correct blending!
			const EnvironmentProbeComponent& probe = vis.scene->probes[probeIndex];

			shaderentity = {}; // zero out!
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->probes.GetEntity(probeIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_ENVMAP);
			shaderentity.position = probe.position;
			shaderentity.SetRange(probe.range);

			shaderentity.SetIndices(matrixCounter, (uint32_t)probe.textureIndex);
			shadermatrix = XMLoadFloat4x4(&probe.inverseMatrix);

			std::memcpy(matrixArray + matrixCounter, &shadermatrix, sizeof(XMMATRIX));
			matrixCounter++;

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
		}

		// Write lights into entity array:
		const XMFLOAT2 atlas_dim_rcp = XMFLOAT2(1.0f / float(shadowMapAtlas.desc.width), 1.0f / float(shadowMapAtlas.desc.height));
		for (uint32_t lightIndex : vis.visibleLights)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			ShaderEntity shaderentity = {};

			const LightComponent& light = vis.scene->lights[lightIndex];

			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->lights.GetEntity(lightIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(light.GetType());
			shaderentity.position = light.position;
			shaderentity.SetRange(light.GetRange());
			shaderentity.SetColor(float4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity, 1));

			// mark as no shadow by default:
			shaderentity.indices = ~0;

			bool shadow = light.IsCastingShadow() && !light.IsStatic();

			if (GetRaytracedShadowsEnabled() && shadow)
			{
				shaderentity.SetIndices(matrixCounter, 0);
			}
			else if(shadow)
			{
				shadow = light.IsCastingShadow() && !light.IsStatic();
				if (shadow)
				{
					shaderentity.shadowAtlasMulAdd.x = light.shadow_rect.w * atlas_dim_rcp.x;
					shaderentity.shadowAtlasMulAdd.y = light.shadow_rect.h * atlas_dim_rcp.y;
					shaderentity.shadowAtlasMulAdd.z = light.shadow_rect.x * atlas_dim_rcp.x;
					shaderentity.shadowAtlasMulAdd.w = light.shadow_rect.y * atlas_dim_rcp.y;
					shaderentity.SetIndices(matrixCounter, 0);
				}
			}

			switch (light.GetType())
			{
			case LightComponent::DIRECTIONAL:
			{
				shaderentity.SetDirection(light.direction);

				if (shadow)
				{
					SHCAM shcams[CASCADE_COUNT];
					CreateDirLightShadowCams(light, *vis.camera, shcams, arraysize(shcams));
					std::memcpy(&matrixArray[matrixCounter++], &shcams[0].view_projection, sizeof(XMMATRIX));
					std::memcpy(&matrixArray[matrixCounter++], &shcams[1].view_projection, sizeof(XMMATRIX));
					std::memcpy(&matrixArray[matrixCounter++], &shcams[2].view_projection, sizeof(XMMATRIX));
				}
			}
			break;
			case LightComponent::POINT:
			{
				if (shadow)
				{
					const float FarZ = 0.1f;	// watch out: reversed depth buffer! Also, light near plane is constant for simplicity, this should match on cpu side!
					const float NearZ = std::max(1.0f, light.GetRange()); // watch out: reversed depth buffer!
					const float fRange = FarZ / (FarZ - NearZ);
					const float cubemapDepthRemapNear = fRange;
					const float cubemapDepthRemapFar = -fRange * NearZ;
					shaderentity.SetCubeRemapNear(cubemapDepthRemapNear);
					shaderentity.SetCubeRemapFar(cubemapDepthRemapFar);
				}
			}
			break;
			case LightComponent::SPOT:
			{
				const float outerConeAngle = light.outerConeAngle;
				const float innerConeAngle = std::min(light.innerConeAngle, outerConeAngle);
				const float outerConeAngleCos = std::cos(outerConeAngle);
				const float innerConeAngleCos = std::cos(innerConeAngle);

				// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual#inner-and-outer-cone-angles
				const float lightAngleScale = 1.0f / std::max(0.001f, innerConeAngleCos - outerConeAngleCos);
				const float lightAngleOffset = -outerConeAngleCos * lightAngleScale;

				shaderentity.SetConeAngleCos(outerConeAngleCos);
				shaderentity.SetAngleScale(lightAngleScale);
				shaderentity.SetAngleOffset(lightAngleOffset);
				shaderentity.SetDirection(light.direction);

				if (shadow)
				{
					SHCAM shcam;
					CreateSpotLightShadowCam(light, shcam);
					std::memcpy(&matrixArray[matrixCounter++], &shcam.view_projection, sizeof(XMMATRIX));
				}
			}
			break;
			}

			if (light.IsStatic())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_STATIC);
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
		}

		// Write force fields into entity array:
		for (size_t i = 0; i < vis.scene->forces.GetCount(); ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				assert(0); // too many entities!
				entityCounter--;
				break;
			}
			ShaderEntity shaderentity = {};

			const ForceFieldComponent& force = vis.scene->forces[i];

			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->forces.GetEntity(i);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(force.type);
			shaderentity.position = force.position;
			shaderentity.SetGravity(force.gravity);
			shaderentity.SetRange(std::max(0.001f, force.GetRange()));
			// The default planar force field is facing upwards, and thus the pull direction is downwards:
			shaderentity.SetDirection(force.direction);

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
		}

		// Issue GPU entity array update:
		if (entityCounter > 0)
		{
			device->CopyBuffer(
				&resourceBuffers[RBTYPE_ENTITYARRAY],
				0,
				&allocation_entityarray.buffer,
				allocation_entityarray.offset,
				sizeof(ShaderEntity) * entityCounter,
				cmd
			);
			barrier_stack.push_back(GPUBarrier::Buffer(&resourceBuffers[RBTYPE_ENTITYARRAY], ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
		}
		if (matrixCounter > 0)
		{
			device->CopyBuffer(
				&resourceBuffers[RBTYPE_MATRIXARRAY],
				0,
				&allocation_matrixarray.buffer,
				allocation_matrixarray.offset,
				sizeof(XMMATRIX) * matrixCounter,
				cmd
			);
			barrier_stack.push_back(GPUBarrier::Buffer(&resourceBuffers[RBTYPE_MATRIXARRAY], ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
		}

	}

	// Upload bones for skinning to shader:
	for (size_t i = 0; i < vis.scene->armatures.GetCount(); ++i)
	{
		const ArmatureComponent& armature = vis.scene->armatures[i];
		device->UpdateBuffer(&armature.boneBuffer, armature.boneData.data(), cmd);

		barrier_stack.push_back(GPUBarrier::Buffer(&armature.boneBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	// Soft body updates:
	for (size_t i = 0; i < vis.scene->softbodies.GetCount(); ++i)
	{
		Entity entity = vis.scene->softbodies.GetEntity(i);
		const SoftBodyPhysicsComponent& softbody = vis.scene->softbodies[i];

		const MeshComponent* mesh = vis.scene->meshes.GetComponent(entity);
		if (mesh != nullptr && mesh->streamoutBuffer.IsValid() && !softbody.vertex_positions_simulation.empty())
		{
			GraphicsDevice::GPUAllocation allocation = device->AllocateGPU(mesh->so_pos_nor_wind.size + mesh->so_tan.size, cmd);
			std::memcpy(allocation.data, softbody.vertex_positions_simulation.data(), mesh->so_pos_nor_wind.size);
			device->CopyBuffer(&mesh->streamoutBuffer, mesh->so_pos_nor_wind.offset, &allocation.buffer, allocation.offset, mesh->so_pos_nor_wind.size, cmd);
			if (!softbody.vertex_tangents_simulation.empty())
			{
				std::memcpy((uint8_t*)allocation.data + mesh->so_pos_nor_wind.size, softbody.vertex_tangents_simulation.data(), mesh->so_tan.size);
				device->CopyBuffer(&mesh->streamoutBuffer, mesh->so_tan.offset, &allocation.buffer, allocation.offset + mesh->so_pos_nor_wind.size, mesh->so_tan.size, cmd);
			}
			barrier_stack.push_back(GPUBarrier::Buffer(&mesh->streamoutBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
		}
	}

	// Flush buffer updates:
	barrier_stack_flush(cmd);

	wi::profiler::EndRange(prof_updatebuffer_cpu);
	wi::profiler::EndRange(prof_updatebuffer_gpu);

	BindCommonResources(cmd);

	auto range = wi::profiler::BeginRangeGPU("Skinning", cmd);
	device->EventBegin("Skinning", cmd);
	{
		for (size_t i = 0; i < vis.scene->meshes.GetCount(); ++i)
		{
			Entity entity = vis.scene->meshes.GetEntity(i);
			const MeshComponent& mesh = vis.scene->meshes[i];

			if (mesh.dirty_morph)
			{
				mesh.dirty_morph = false;
				GraphicsDevice::GPUAllocation allocation = device->AllocateGPU(mesh.vb_pos_nor_wind.size, cmd);
				std::memcpy(allocation.data, mesh.vertex_positions_morphed.data(), mesh.vb_pos_nor_wind.size);
				device->CopyBuffer(&mesh.generalBuffer, mesh.vb_pos_nor_wind.offset, &allocation.buffer, allocation.offset, mesh.vb_pos_nor_wind.size, cmd);
				barrier_stack.push_back(GPUBarrier::Buffer(&mesh.generalBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
			}

			if (mesh.IsSkinned() && vis.scene->armatures.Contains(mesh.armatureID))
			{
				const SoftBodyPhysicsComponent* softbody = vis.scene->softbodies.GetComponent(entity);
				if (softbody != nullptr && softbody->physicsobject != nullptr)
				{
					// If soft body simulation is active, don't perform skinning.
					//	(Soft body animated vertices are skinned in simulation phase by physics system)
					continue;
				}

				const ArmatureComponent& armature = *vis.scene->armatures.GetComponent(mesh.armatureID);

				device->BindComputeShader(&shaders[CSTYPE_SKINNING], cmd);

				SkinningPushConstants push;
				push.bonebuffer_index = armature.descriptor_srv;
				push.vb_pos_nor_wind = mesh.vb_pos_nor_wind.descriptor_srv;
				push.vb_tan = mesh.vb_tan.descriptor_srv;
				push.vb_bon = mesh.vb_bon.descriptor_srv;
				push.so_pos_nor_wind = mesh.so_pos_nor_wind.descriptor_uav;
				push.so_tan = mesh.so_tan.descriptor_uav;
				device->PushConstants(&push, sizeof(push), cmd);

				device->Dispatch(((uint32_t)mesh.vertex_positions.size() + 63) / 64, 1, 1, cmd);

				barrier_stack.push_back(GPUBarrier::Buffer(&mesh.streamoutBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));

			}

		}

		barrier_stack_flush(cmd);

	}
	device->EventEnd(cmd);
	wi::profiler::EndRange(range); // skinning

	// Hair particle systems GPU simulation:
	//	(This must be non-async too, as prepass will render hairs!)
	if (!vis.visibleHairs.empty() && frameCB.delta_time > 0)
	{
		range = wi::profiler::BeginRangeGPU("HairParticles - Simulate", cmd);
		for (uint32_t hairIndex : vis.visibleHairs)
		{
			const wi::HairParticleSystem& hair = vis.scene->hairs[hairIndex];
			const MeshComponent* mesh = vis.scene->meshes.GetComponent(hair.meshID);

			if (mesh != nullptr)
			{
				Entity entity = vis.scene->hairs.GetEntity(hairIndex);
				size_t materialIndex = vis.scene->materials.GetIndex(entity);
				const MaterialComponent& material = vis.scene->materials[materialIndex];

				hair.UpdateGPU((uint32_t)vis.scene->objects.GetCount() + hairIndex, *mesh, material, cmd);
			}
		}
		wi::profiler::EndRange(range);
	}

	// Impostor prepare:
	if (vis.scene->instanceArraySize > 0 && vis.scene->meshletBuffer.IsValid())
	{
		device->EventBegin("Impostor prepare", cmd);
		auto range = wi::profiler::BeginRangeGPU("Impostor prepare", cmd);

		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->impostorIndirectBuffer, ResourceState::INDIRECT_ARGUMENT, ResourceState::COPY_DST));
		barrier_stack_flush(cmd);
		IndirectDrawArgsIndexedInstanced clear_indirect = {};
		clear_indirect.index_count_per_instance = 0;
		clear_indirect.instance_count = 1;
		clear_indirect.start_index_location = 0;
		clear_indirect.base_vertex_location = 0;
		clear_indirect.start_instance_location = 0;
		device->UpdateBuffer(&vis.scene->impostorIndirectBuffer, &clear_indirect, cmd, sizeof(clear_indirect), 0);
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->impostorIndirectBuffer, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS));
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS));
		barrier_stack_flush(cmd);

		device->BindComputeShader(&shaders[CSTYPE_IMPOSTOR_PREPARE], cmd);
		device->BindUAV(&vis.scene->impostorBuffer, 0, cmd, vis.scene->impostor_ib.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 1, cmd, vis.scene->impostor_vb.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 2, cmd, vis.scene->impostor_data.subresource_uav);
		device->BindUAV(&vis.scene->impostorIndirectBuffer, 3, cmd);

		uint object_count = (uint)vis.scene->objects.GetCount();
		device->PushConstants(&object_count, sizeof(object_count), cmd);

		device->Dispatch((object_count + 63u) / 64u, 1, 1, cmd);

		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
		barrier_stack.push_back(GPUBarrier::Buffer(&vis.scene->impostorIndirectBuffer,ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT));
		barrier_stack_flush(cmd);

		wi::profiler::EndRange(range);
		device->EventEnd(cmd);
	}

	// Meshlets:
	if(vis.scene->instanceArraySize > 0 && vis.scene->meshletBuffer.IsValid())
	{
		device->EventBegin("Meshlet prepare", cmd);
		auto range = wi::profiler::BeginRangeGPU("Meshlet prepare", cmd);
		device->BindComputeShader(&shaders[CSTYPE_MESHLET_PREPARE], cmd);

		const GPUResource* uavs[] = {
			&vis.scene->meshletBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch((uint32_t)vis.scene->instanceArraySize, 1, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&vis.scene->meshletBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
		wi::profiler::EndRange(range);
		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}


void UpdateRenderDataAsync(
	const Visibility& vis,
	const FrameCB& frameCB,
	CommandList cmd
)
{
	device->EventBegin("UpdateRenderDataAsync", cmd);

	BindCommonResources(cmd);

	if (vis.scene->weather.IsRealisticSky())
	{
		// Render Atmospheric Scattering textures for lighting and sky
		RenderAtmosphericScatteringTextures(cmd);
	}

	// Precompute static volumetric cloud textures:
	if (!volumetric_clouds_precomputed && vis.scene->weather.IsVolumetricClouds())
	{
		// Shape Noise pass:
		{
			device->EventBegin("Shape Noise", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHAPENOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_shapeNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_shapeNoise, texture_shapeNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 8;
			const int noiseThreadXY = static_cast<uint32_t>(std::ceil(texture_shapeNoise.GetDesc().width / threadSize));
			const int noiseThreadZ = static_cast<uint32_t>(std::ceil(texture_shapeNoise.GetDesc().depth / threadSize));

			device->Dispatch(noiseThreadXY, noiseThreadXY, noiseThreadZ, cmd);

			device->EventEnd(cmd);
		}

		// Detail Noise pass:
		{
			device->EventBegin("Detail Noise", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_DETAILNOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_detailNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_detailNoise, texture_detailNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 8;
			const int noiseThreadXYZ = static_cast<uint32_t>(std::ceil(texture_detailNoise.GetDesc().width / threadSize));

			device->Dispatch(noiseThreadXYZ, noiseThreadXYZ, noiseThreadXYZ, cmd);

			device->EventEnd(cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&texture_shapeNoise, ResourceState::UNORDERED_ACCESS, texture_shapeNoise.desc.layout),
				GPUBarrier::Image(&texture_detailNoise, ResourceState::UNORDERED_ACCESS, texture_detailNoise.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		// Generate mip chains for 3D textures:
		GenerateMipChain(texture_shapeNoise, MIPGENFILTER_LINEAR, cmd);
		GenerateMipChain(texture_detailNoise, MIPGENFILTER_LINEAR, cmd);

		// Curl Noise pass:
		{
			device->EventBegin("Curl Map", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_CURLNOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_curlNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_curlNoise, texture_curlNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 16;
			const int curlRes = texture_curlNoise.GetDesc().width;
			const int curlThread = static_cast<uint32_t>(std::ceil(curlRes / threadSize));

			device->Dispatch(curlThread, curlThread, 1, cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_curlNoise, ResourceState::UNORDERED_ACCESS, texture_curlNoise.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}

		// Weather Map pass:
		{
			device->EventBegin("Weather Map", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_WEATHERMAP], cmd);

			const GPUResource* uavs[] = {
				&texture_weatherMap,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_weatherMap, texture_weatherMap.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 16;
			const int weatherMapRes = texture_weatherMap.GetDesc().width;
			const int weatherThread = static_cast<uint32_t>(std::ceil(weatherMapRes / threadSize));

			device->Dispatch(weatherThread, weatherThread, 1, cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&texture_weatherMap, ResourceState::UNORDERED_ACCESS, texture_weatherMap.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}
		volumetric_clouds_precomputed = true;
	}

	// GPU Particle systems simulation/sorting/culling:
	if (!vis.visibleEmitters.empty() && frameCB.delta_time > 0)
	{
		auto range = wi::profiler::BeginRangeGPU("EmittedParticles - Simulate", cmd);
		for (uint32_t emitterIndex : vis.visibleEmitters)
		{
			const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
			Entity entity = vis.scene->emitters.GetEntity(emitterIndex);
			const TransformComponent& transform = *vis.scene->transforms.GetComponent(entity);
			const MaterialComponent& material = *vis.scene->materials.GetComponent(entity);
			const MeshComponent* mesh = vis.scene->meshes.GetComponent(emitter.meshID);
			const uint32_t instanceIndex = uint32_t(vis.scene->objects.GetCount() + vis.scene->hairs.GetCount()) + emitterIndex;

			emitter.UpdateGPU(instanceIndex, transform, mesh, cmd);
		}
		wi::profiler::EndRange(range);
	}

	// Compute water simulation:
	if (vis.scene->weather.IsOceanEnabled())
	{
		if (!GetOcclusionCullingEnabled() || !vis.scene->ocean.IsOccluded())
		{
			auto range = wi::profiler::BeginRangeGPU("Ocean - Simulate", cmd);
			vis.scene->ocean.UpdateDisplacementMap(vis.scene->weather.oceanParameters, cmd);
			wi::profiler::EndRange(range);
		}
	}

	device->EventEnd(cmd);
}

void UpdateRaytracingAccelerationStructures(const Scene& scene, CommandList cmd)
{
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		if (!scene.TLAS.IsValid())
			return;

		device->CopyBuffer(
			&scene.TLAS.desc.top_level.instance_buffer,
			0,
			&scene.TLAS_instancesUpload[device->GetBufferIndex()],
			0,
			scene.TLAS.desc.top_level.instance_buffer.desc.size,
			cmd
		);

		// BLAS:
		{
			auto rangeCPU = wi::profiler::BeginRangeCPU("BLAS Update (CPU)");
			auto range = wi::profiler::BeginRangeGPU("BLAS Update (GPU)", cmd);
			device->EventBegin("BLAS Update", cmd);

			for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
			{
				const MeshComponent& mesh = scene.meshes[i];
				if (mesh.BLAS.IsValid())
				{
					switch (mesh.BLAS_state)
					{
					default:
					case MeshComponent::BLAS_STATE_COMPLETE:
						break;
					case MeshComponent::BLAS_STATE_NEEDS_REBUILD:
						device->BuildRaytracingAccelerationStructure(&mesh.BLAS, cmd, nullptr);
						break;
					case MeshComponent::BLAS_STATE_NEEDS_REFIT:
						device->BuildRaytracingAccelerationStructure(&mesh.BLAS, cmd, &mesh.BLAS);
						break;
					}
					mesh.BLAS_state = MeshComponent::BLAS_STATE_COMPLETE;
				}
			}

			for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
			{
				const wi::HairParticleSystem& hair = scene.hairs[i];

				if (hair.meshID != INVALID_ENTITY && hair.BLAS.IsValid())
				{
					device->BuildRaytracingAccelerationStructure(&hair.BLAS, cmd, nullptr);
				}
			}

			for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
			{
				const wi::EmittedParticleSystem& emitter = scene.emitters[i];

				if (emitter.BLAS.IsValid())
				{
					device->BuildRaytracingAccelerationStructure(&emitter.BLAS, cmd, nullptr);
				}
			}

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Buffer(&scene.TLAS.desc.top_level.instance_buffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE_COMPUTE),
					GPUBarrier::Memory(), // sync BLAS
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
			wi::profiler::EndRange(range);
			wi::profiler::EndRange(rangeCPU);
		}

		// TLAS:
		{
			auto rangeCPU = wi::profiler::BeginRangeCPU("TLAS Update (CPU)");
			auto range = wi::profiler::BeginRangeGPU("TLAS Update (GPU)", cmd);
			device->EventBegin("TLAS Update", cmd);

			device->BuildRaytracingAccelerationStructure(&scene.TLAS, cmd, nullptr);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(&scene.TLAS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
			wi::profiler::EndRange(range);
			wi::profiler::EndRange(rangeCPU);
		}
	}
	else
	{
		BindCommonResources(cmd);
		scene.BVH.Build(scene, cmd);
	}

	scene.acceleration_structure_update_requested = false;
}


void OcclusionCulling_Reset(const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	int query_write = vis.scene->queryheap_idx;
	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;

	device->QueryReset(
		&queryHeap,
		0,
		queryHeap.desc.query_count,
		cmd
	);
}
void OcclusionCulling_Render(const CameraComponent& camera, const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	auto range = wi::profiler::BeginRangeGPU("Occlusion Culling Render", cmd);

	device->BindPipelineState(&PSO_occlusionquery, cmd);

	XMMATRIX VP = camera.GetViewProjection();

	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;
	int query_write = vis.scene->queryheap_idx;

	if (!vis.visibleObjects.empty())
	{
		device->EventBegin("Occlusion Culling Objects", cmd);

		for (uint32_t instanceIndex : vis.visibleObjects)
		{
			const ObjectComponent& object = vis.scene->objects[instanceIndex];

			int queryIndex = object.occlusionQueries[query_write];
			if (queryIndex >= 0)
			{
				const AABB& aabb = vis.scene->aabb_objects[instanceIndex];
				const XMMATRIX transform = aabb.getAsBoxMatrix() * VP;
				device->PushConstants(&transform, sizeof(transform), cmd);

				// render bounding box to later read the occlusion status
				device->QueryBegin(&queryHeap, queryIndex, cmd);
				device->Draw(14, 0, cmd);
				device->QueryEnd(&queryHeap, queryIndex, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (!vis.visibleLights.empty())
	{
		device->EventBegin("Occlusion Culling Lights", cmd);

		for (uint32_t lightIndex : vis.visibleLights)
		{
			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.occlusionquery >= 0)
			{
				uint32_t queryIndex = (uint32_t)light.occlusionquery;
				const AABB& aabb = vis.scene->aabb_lights[lightIndex];
				const XMMATRIX transform = aabb.getAsBoxMatrix() * VP;
				device->PushConstants(&transform, sizeof(transform), cmd);

				device->QueryBegin(&queryHeap, queryIndex, cmd);
				device->Draw(14, 0, cmd);
				device->QueryEnd(&queryHeap, queryIndex, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (vis.scene->weather.IsOceanEnabled())
	{
		int queryIndex = vis.scene->ocean.occlusionQueries[query_write];
		if (queryIndex >= 0)
		{
			device->EventBegin("Occlusion Culling Ocean", cmd);

			AABB aabb;
			aabb.createFromHalfWidth(
				XMFLOAT3(vis.camera->Eye.x, vis.scene->weather.oceanParameters.waterHeight, vis.camera->Eye.z),
				XMFLOAT3(vis.camera->zFarP, 1, vis.camera->zFarP)
			);
			const XMMATRIX transform = aabb.getAsBoxMatrix() * VP;
			device->PushConstants(&transform, sizeof(transform), cmd);

			device->QueryBegin(&queryHeap, queryIndex, cmd);
			device->Draw(14, 0, cmd);
			device->QueryEnd(&queryHeap, queryIndex, cmd);

			device->EventEnd(cmd);
		}
	}

	wi::profiler::EndRange(range); // Occlusion Culling Render
}
void OcclusionCulling_Resolve(const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	int query_write = vis.scene->queryheap_idx;
	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;
	uint32_t queryCount = vis.scene->queryAllocator.load();

	// Resolve into readback buffer:
	device->QueryResolve(
		&queryHeap,
		0,
		queryCount,
		&vis.scene->queryResultBuffer[query_write],
		0ull,
		cmd
	);

	if (device->CheckCapability(GraphicsDeviceCapability::PREDICATION))
	{
		// Resolve into predication buffer:
		device->QueryResolve(
			&queryHeap,
			0,
			queryCount,
			&vis.scene->queryPredicationBuffer,
			0ull,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&vis.scene->queryPredicationBuffer, ResourceState::COPY_DST, ResourceState::PREDICATION),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}
}

void DrawWaterRipples(const Visibility& vis, CommandList cmd)
{
	// remove camera jittering
	CameraComponent cam = *vis.camera;
	cam.jitter = XMFLOAT2(0, 0);
	cam.UpdateCamera();
	const XMMATRIX VP = cam.GetViewProjection();

	XMVECTOR vvv = abs(vis.reflectionPlane.x) > 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
	XMVECTOR dir = XMLoadFloat4(&vis.reflectionPlane);
	XMMATRIX R = XMMatrixLookToLH(XMVectorZero(), dir, XMVector3Cross(vvv, dir));

	device->EventBegin("Water Ripples", cmd);
	for(auto& x : vis.scene->waterRipples)
	{
		x.params.customRotation = &R;
		x.params.customProjection = &VP;
		x.Draw(cmd);
	}
	device->EventEnd(cmd);
}



void DrawSoftParticles(
	const Visibility& vis,
	const Texture& lineardepth,
	bool distortion, 
	CommandList cmd
)
{
	size_t emitterCount = vis.visibleEmitters.size();
	if (emitterCount == 0)
	{
		return;
	}
	auto range = distortion ?
		wi::profiler::BeginRangeGPU("EmittedParticles - Render (Distortion)", cmd) :
		wi::profiler::BeginRangeGPU("EmittedParticles - Render", cmd);

	BindCommonResources(cmd);

	// Sort emitters based on distance:
	assert(emitterCount < 0x0000FFFF); // watch out for sorting hash truncation!
	static thread_local wi::vector<uint32_t> emitterSortingHashes;
	emitterSortingHashes.resize(emitterCount);
	for (size_t i = 0; i < emitterCount; ++i)
	{
		const uint32_t emitterIndex = vis.visibleEmitters[i];
		const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
		float distance = wi::math::DistanceEstimated(emitter.center, vis.camera->Eye);
		emitterSortingHashes[i] = 0;
		emitterSortingHashes[i] |= (uint32_t)i & 0x0000FFFF;
		emitterSortingHashes[i] |= (uint32_t)XMConvertFloatToHalf(distance) << 16u;
	}
	std::sort(emitterSortingHashes.begin(), emitterSortingHashes.end(), std::greater<uint32_t>());

	for (size_t i = 0; i < emitterCount; ++i)
	{
		const uint32_t emitterIndex = vis.visibleEmitters[emitterSortingHashes[i] & 0x0000FFFF];
		const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
		const Entity entity = vis.scene->emitters.GetEntity(emitterIndex);
		const MaterialComponent& material = *vis.scene->materials.GetComponent(entity);

		if (distortion && emitter.shaderType == wi::EmittedParticleSystem::SOFT_DISTORTION)
		{
			emitter.Draw(material, cmd);
		}
		else if (!distortion && (emitter.shaderType == wi::EmittedParticleSystem::SOFT || emitter.shaderType == wi::EmittedParticleSystem::SOFT_LIGHTING || emitter.shaderType == wi::EmittedParticleSystem::SIMPLE || IsWireRender()))
		{
			emitter.Draw(material, cmd);
		}
	}

	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);

	wi::profiler::EndRange(range);
}
void DrawLightVisualizers(
	const Visibility& vis,
	CommandList cmd
)
{
	if (!vis.visibleLights.empty())
	{
		device->EventBegin("Light Visualizer Render", cmd);

		BindCommonResources(cmd);

		XMMATRIX camrot = XMLoadFloat3x3(&vis.camera->rotationMatrix);
		XMMATRIX VP = vis.camera->GetViewProjection();

		for (int type = LightComponent::POINT; type < LightComponent::LIGHTTYPE_COUNT; ++type)
		{
			device->BindPipelineState(&PSO_lightvisualizer[type], cmd);

			for (uint32_t lightIndex : vis.visibleLights)
			{
				const LightComponent& light = vis.scene->lights[lightIndex];

				if (light.GetType() == type && light.IsVisualizerEnabled())
				{

					VolumeLightCB lcb;
					lcb.xLightColor = XMFLOAT4(light.color.x, light.color.y, light.color.z, 1);
					lcb.xLightEnerdis = XMFLOAT4(light.intensity, light.GetRange(), light.outerConeAngle, light.intensity);

					if (type == LightComponent::POINT)
					{
						lcb.xLightEnerdis.w = light.GetRange() * 0.025f; // scale
						XMStoreFloat4x4(&lcb.xLightWorld,
							XMMatrixScaling(lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, lcb.xLightEnerdis.w)*
							camrot*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
						);

						device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

						device->Draw(108, 0, cmd); // circle
					}
					else if (type == LightComponent::SPOT)
					{
						if (light.innerConeAngle > 0)
						{
							float coneS = (float)(std::min(light.innerConeAngle, light.outerConeAngle) * 2 / XM_PIDIV4);
							lcb.xLightEnerdis.w = light.GetRange() * 0.1f; // scale
							XMStoreFloat4x4(&lcb.xLightWorld,
								XMMatrixScaling(coneS * lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, coneS * lcb.xLightEnerdis.w) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
							);

							device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

							device->Draw(192, 0, cmd); // cone
						}

						float coneS = (float)(light.outerConeAngle * 2 / XM_PIDIV4);
						lcb.xLightEnerdis.w = light.GetRange() * 0.1f; // scale
						XMStoreFloat4x4(&lcb.xLightWorld,
							XMMatrixScaling(coneS*lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, coneS*lcb.xLightEnerdis.w)*
							XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
						);

						device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

						device->Draw(192, 0, cmd); // cone
					}
				}
			}

		}

		device->EventEnd(cmd);

	}
}
void DrawVolumeLights(
	const Visibility& vis,
	CommandList cmd
)
{

	if (!vis.visibleLights.empty())
	{
		device->EventBegin("Volumetric Light Render", cmd);

		BindCommonResources(cmd);

		XMMATRIX VP = vis.camera->GetViewProjection();

		for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
		{
			const PipelineState& pso = PSO_volumetriclight[type];

			if (!pso.IsValid())
			{
				continue;
			}

			device->BindPipelineState(&pso, cmd);

			for (size_t i = 0; i < vis.visibleLights.size(); ++i)
			{
				const uint32_t lightIndex = vis.visibleLights[i];
				const LightComponent& light = vis.scene->lights[lightIndex];
				if (light.GetType() == type && light.IsVolumetricsEnabled())
				{

					switch (type)
					{
					case LightComponent::DIRECTIONAL:
					{
						MiscCB miscCb;
						miscCb.g_xColor.x = float(i);
						device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

						device->Draw(3, 0, cmd); // full screen triangle
					}
					break;
					case LightComponent::POINT:
					{
						MiscCB miscCb;
						miscCb.g_xColor.x = float(i);
						float sca = light.GetRange() + 1;
						XMStoreFloat4x4(&miscCb.g_xTransform, XMMatrixScaling(sca, sca, sca)*XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) * VP);
						device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

						device->Draw(240, 0, cmd); // icosphere
					}
					break;
					case LightComponent::SPOT:
					{
						MiscCB miscCb;
						miscCb.g_xColor.x = float(i);
						const float coneS = (const float)(light.outerConeAngle * 2 / XM_PIDIV4);
						XMStoreFloat4x4(&miscCb.g_xTransform, 
							XMMatrixScaling(coneS*light.GetRange(), light.GetRange(), coneS*light.GetRange())*
							XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) *
							VP
						);
						device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

						device->Draw(192, 0, cmd); // cone
					}
					break;
					}

				}
			}

		}

		device->EventEnd(cmd);
	}


}
void DrawLensFlares(
	const Visibility& vis,
	CommandList cmd,
	const Texture* texture_directional_occlusion
)
{
	if (IsWireRender())
		return;

	device->EventBegin("Lens Flares", cmd);

	BindCommonResources(cmd);

	for (uint32_t lightIndex : vis.visibleLights)
	{
		const LightComponent& light = vis.scene->lights[lightIndex];

		if (!light.lensFlareRimTextures.empty())
		{
			XMVECTOR POS;

			if (light.GetType() == LightComponent::DIRECTIONAL)
			{
				// directional light flare will be placed at infinite position along direction vector:
				XMVECTOR D = XMVector3Normalize(-XMVector3Transform(XMVectorSet(0, 1, 0, 1), XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))));
				if (XMVectorGetX(XMVector3Dot(D, XMVectorSet(0, -1, 0, 0))) < 0)
					continue; // sun below horizon, skip lensflare
				POS = vis.camera->GetEye() + D * -vis.camera->zFarP;

				// Directional light can use occlusion texture (eg. clouds):
				if (texture_directional_occlusion == nullptr)
				{
					device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
				}
				else
				{
					device->BindResource(texture_directional_occlusion, 0, cmd);
				}
			}
			else
			{
				// point and spotlight flare will be placed to the source position:
				POS = XMLoadFloat3(&light.position);

				// not using occlusion texture
				device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
			}

			if (XMVectorGetX(XMVector3Dot(XMVectorSubtract(POS, vis.camera->GetEye()), vis.camera->GetAt())) > 0) // check if the camera is facing towards the flare or not
			{
				device->BindPipelineState(&PSO_lensflare, cmd);

				// Get the screen position of the flare:
				XMVECTOR flarePos = XMVector3Project(POS, 0, 0, 1, 1, 1, 0, vis.camera->GetProjection(), vis.camera->GetView(), XMMatrixIdentity());

				LensFlarePush cb;
				XMStoreFloat3(&cb.xLensFlarePos, flarePos);

				uint32_t i = 0;
				for (auto& x : light.lensFlareRimTextures)
				{
					if (x.IsValid())
					{
						// pre-baked offsets
						// These values work well for me, but should be tweakable
						static const float mods[] = { 1.0f,0.55f,0.4f,0.1f,-0.1f,-0.3f,-0.5f };
						if (i >= arraysize(mods))
							break;

						cb.xLensFlareOffset = mods[i];
						cb.xLensFlareSize.x = (float)x.GetTexture().desc.width;
						cb.xLensFlareSize.y = (float)x.GetTexture().desc.height;

						device->PushConstants(&cb, sizeof(cb), cmd);

						device->BindResource(&x.GetTexture(), 1, cmd);
						device->Draw(4, 0, cmd);
						i++;
					}
				}
			}

		}

	}

	device->EventEnd(cmd);
}


void SetShadowProps2D(int resolution)
{
	max_shadow_resolution_2D = resolution;
}
void SetShadowPropsCube(int resolution)
{
	max_shadow_resolution_cube = resolution;
}

void DrawShadowmaps(
	const Visibility& vis,
	CommandList cmd
)
{
	if (IsWireRender())
		return;

	if (!vis.visibleLights.empty() && renderpass_shadowMapAtlas.IsValid())
	{
		device->EventBegin("DrawShadowmaps", cmd);
		auto range_cpu = wi::profiler::BeginRangeCPU("Shadowmap Rendering");
		auto range_gpu = wi::profiler::BeginRangeGPU("Shadowmap Rendering", cmd);

		const bool predicationRequest =
			device->CheckCapability(GraphicsDeviceCapability::PREDICATION) &&
			GetOcclusionCullingEnabled();

		BindCommonResources(cmd);

		BoundingFrustum cam_frustum;
		BoundingFrustum::CreateFromMatrix(cam_frustum, vis.camera->GetProjection());
		std::swap(cam_frustum.Near, cam_frustum.Far);
		cam_frustum.Transform(cam_frustum, vis.camera->GetInvView());
		XMStoreFloat4(&cam_frustum.Orientation, XMQuaternionNormalize(XMLoadFloat4(&cam_frustum.Orientation)));

		static thread_local RenderQueue renderQueue;

		device->RenderPassBegin(&renderpass_shadowMapAtlas, cmd);

		for (uint32_t lightIndex : vis.visibleLights)
		{
			const LightComponent& light = vis.scene->lights[lightIndex];
			
			bool shadow = light.IsCastingShadow() && !light.IsStatic();
			if (!shadow)
			{
				continue;
			}

			switch (light.GetType())
			{
			case LightComponent::DIRECTIONAL:
			{
				SHCAM shcams[CASCADE_COUNT];
				CreateDirLightShadowCams(light, *vis.camera, shcams, arraysize(shcams));

				for (uint32_t cascade = 0; cascade < CASCADE_COUNT; ++cascade)
				{
					renderQueue.init();
					bool transparentShadowsRequested = false;
					for (size_t i = 0; i < vis.scene->aabb_objects.GetCount(); ++i)
					{
						const AABB& aabb = vis.scene->aabb_objects[i];
						if ((aabb.layerMask & vis.layerMask) && shcams[cascade].frustum.CheckBoxFast(aabb))
						{
							const ObjectComponent& object = vis.scene->objects[i];
							if (object.IsRenderable() && object.IsCastingShadow() && (cascade < (CASCADE_COUNT - object.cascadeMask)))
							{
								Entity cullable_entity = vis.scene->aabb_objects.GetEntity(i);

								renderQueue.add(object.mesh_index, uint32_t(i), 0);

								if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT || object.GetRenderTypes() & RENDERTYPE_WATER)
								{
									transparentShadowsRequested = true;
								}
							}
						}
					}

					if (!renderQueue.empty())
					{
						CameraCB cb;
						XMStoreFloat4x4(&cb.view_projection, shcams[cascade].view_projection);
						device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

						Viewport vp;
						vp.top_left_x = float(light.shadow_rect.x + cascade * light.shadow_rect.w);
						vp.top_left_y = float(light.shadow_rect.y);
						vp.width = float(light.shadow_rect.w);
						vp.height = float(light.shadow_rect.h);
						vp.min_depth = 0.0f;
						vp.max_depth = 1.0f;
						device->BindViewports(1, &vp, cmd);

						RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, RENDERTYPE_OPAQUE, cmd);
						if (GetTransparentShadowsEnabled() && transparentShadowsRequested)
						{
							RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, cmd);
						}
					}

				}
			}
			break;
			case LightComponent::SPOT:
			{
				SHCAM shcam;
				CreateSpotLightShadowCam(light, shcam);
				if (!cam_frustum.Intersects(shcam.boundingfrustum))
					break;

				renderQueue.init();
				bool transparentShadowsRequested = false;
				for (size_t i = 0; i < vis.scene->aabb_objects.GetCount(); ++i)
				{
					const AABB& aabb = vis.scene->aabb_objects[i];
					if ((aabb.layerMask & vis.layerMask) && shcam.frustum.CheckBoxFast(aabb))
					{
						const ObjectComponent& object = vis.scene->objects[i];
						if (object.IsRenderable() && object.IsCastingShadow())
						{
							Entity cullable_entity = vis.scene->aabb_objects.GetEntity(i);

							renderQueue.add(object.mesh_index, uint32_t(i), 0);

							if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT || object.GetRenderTypes() & RENDERTYPE_WATER)
							{
								transparentShadowsRequested = true;
							}
						}
					}
				}
				if (!renderQueue.empty())
				{
					if (predicationRequest && light.occlusionquery >= 0)
						device->PredicationBegin(
							&vis.scene->queryPredicationBuffer,
							(uint64_t)light.occlusionquery * sizeof(uint64_t),
							PredicationOp::EQUAL_ZERO,
							cmd
						);

					CameraCB cb;
					XMStoreFloat4x4(&cb.view_projection, shcam.view_projection);
					device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

					Viewport vp;
					vp.top_left_x = float(light.shadow_rect.x);
					vp.top_left_y = float(light.shadow_rect.y);
					vp.width = float(light.shadow_rect.w);
					vp.height = float(light.shadow_rect.h);
					vp.min_depth = 0.0f;
					vp.max_depth = 1.0f;
					device->BindViewports(1, &vp, cmd);

					RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, RENDERTYPE_OPAQUE, cmd);
					if (GetTransparentShadowsEnabled() && transparentShadowsRequested)
					{
						RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, cmd);
					}

					if (predicationRequest && light.occlusionquery >= 0)
						device->PredicationEnd(cmd);
				}

			}
			break;
			case LightComponent::POINT:
			{
				Sphere boundingsphere(light.position, light.GetRange());

				renderQueue.init();
				bool transparentShadowsRequested = false;
				for (size_t i = 0; i < vis.scene->aabb_objects.GetCount(); ++i)
				{
					const AABB& aabb = vis.scene->aabb_objects[i];
					if ((aabb.layerMask & vis.layerMask) && boundingsphere.intersects(aabb))
					{
						const ObjectComponent& object = vis.scene->objects[i];
						if (object.IsRenderable() && object.IsCastingShadow())
						{
							Entity cullable_entity = vis.scene->aabb_objects.GetEntity(i);

							renderQueue.add(object.mesh_index, uint32_t(i), 0);

							if (object.GetRenderTypes() & RENDERTYPE_TRANSPARENT || object.GetRenderTypes() & RENDERTYPE_WATER)
							{
								transparentShadowsRequested = true;
							}
						}
					}
				}
				if (!renderQueue.empty())
				{
					if (predicationRequest && light.occlusionquery >= 0)
						device->PredicationBegin(
							&vis.scene->queryPredicationBuffer,
							(uint64_t)light.occlusionquery * sizeof(uint64_t),
							PredicationOp::EQUAL_ZERO,
							cmd
						);

					const float zNearP = 0.1f;
					const float zFarP = std::max(1.0f, light.GetRange());
					SHCAM cameras[6];
					CreateCubemapCameras(light.position, zNearP, zFarP, cameras, arraysize(cameras));
					Viewport vp[arraysize(cameras)];
					Frustum frusta[arraysize(cameras)];
					uint32_t frustum_count = 0;

					CubemapRenderCB cb;
					for (uint32_t shcam = 0; shcam < arraysize(cameras); ++shcam)
					{
						if (cam_frustum.Intersects(cameras[shcam].boundingfrustum))
						{
							XMStoreFloat4x4(&cb.xCubemapRenderCams[frustum_count].view_projection, cameras[shcam].view_projection);
							cb.xCubemapRenderCams[frustum_count].properties = uint4(shcam, 0, 0, 0);
							frusta[frustum_count] = cameras[shcam].frustum;
							frustum_count++;
						}
						vp[shcam].top_left_x = float(light.shadow_rect.x + shcam * light.shadow_rect.w);
						vp[shcam].top_left_y = float(light.shadow_rect.y);
						vp[shcam].width = float(light.shadow_rect.w);
						vp[shcam].height = float(light.shadow_rect.h);
						vp[shcam].min_depth = 0.0f;
						vp[shcam].max_depth = 1.0f;
					}
					device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(CubemapRenderCB), cmd);
					device->BindViewports(arraysize(vp), vp, cmd);

					RenderMeshes(vis, renderQueue, RENDERPASS_SHADOWCUBE, RENDERTYPE_OPAQUE, cmd, false, frusta, frustum_count);
					if (GetTransparentShadowsEnabled() && transparentShadowsRequested)
					{
						RenderMeshes(vis, renderQueue, RENDERPASS_SHADOWCUBE, RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER, cmd, false, frusta, frustum_count);
					}

					if (predicationRequest && light.occlusionquery >= 0)
						device->PredicationEnd(cmd);
				}

			}
			break;
			} // terminate switch
		}

		device->RenderPassEnd(cmd);

		wi::profiler::EndRange(range_gpu);
		wi::profiler::EndRange(range_cpu);
		device->EventEnd(cmd);
	}
}

void DrawScene(
	const Visibility& vis,
	RENDERPASS renderPass,
	CommandList cmd,
	uint32_t flags
)
{
	const bool opaque = flags & RENDERTYPE_OPAQUE;
	const bool transparent = flags & DRAWSCENE_TRANSPARENT;
	const bool tessellation = (flags & DRAWSCENE_TESSELLATION) && GetTessellationEnabled();
	const bool hairparticle = flags & DRAWSCENE_HAIRPARTICLE;
	const bool impostor = flags & DRAWSCENE_IMPOSTOR;
	const bool occlusion = (flags & DRAWSCENE_OCCLUSIONCULLING) && GetOcclusionCullingEnabled();

	device->EventBegin("DrawScene", cmd);
	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);

	BindCommonResources(cmd);

	if (transparent && vis.scene->weather.IsOceanEnabled())
	{
		if (!occlusion || !vis.scene->ocean.IsOccluded())
		{
			vis.scene->ocean.Render(*vis.camera, vis.scene->weather.oceanParameters, cmd);
		}
	}

	uint32_t renderTypeFlags = 0;
	if (opaque)
	{
		renderTypeFlags |= RENDERTYPE_OPAQUE;
	}
	if (transparent)
	{
		renderTypeFlags |= RENDERTYPE_TRANSPARENT;
		renderTypeFlags |= RENDERTYPE_WATER;
	}

	if (IsWireRender())
	{
		renderTypeFlags = RENDERTYPE_ALL;
	}

	if (hairparticle)
	{
		if (IsWireRender() || !transparent)
		{
			for (uint32_t hairIndex : vis.visibleHairs)
			{
				const wi::HairParticleSystem& hair = vis.scene->hairs[hairIndex];
				Entity entity = vis.scene->hairs.GetEntity(hairIndex);
				const MaterialComponent& material = *vis.scene->materials.GetComponent(entity);

				hair.Draw(material, renderPass, cmd);
			}
		}
	}

	static thread_local RenderQueue renderQueue;
	renderQueue.init();
	for (uint32_t instanceIndex : vis.visibleObjects)
	{
		const ObjectComponent& object = vis.scene->objects[instanceIndex];

		if (occlusion && object.IsOccluded())
			continue;

		if (object.IsRenderable() && (object.GetRenderTypes() & renderTypeFlags))
		{
			const float distance = wi::math::Distance(vis.camera->Eye, object.center);
			if (distance > object.fadeDistance + object.radius)
			{
				continue;
			}
			renderQueue.add(object.mesh_index, instanceIndex, distance);
		}
	}
	if (!renderQueue.empty())
	{
		if (transparent)
		{
			renderQueue.sort_transparent();
		}
		else
		{
			renderQueue.sort_opaque();
		}
		RenderMeshes(vis, renderQueue, renderPass, renderTypeFlags, cmd, tessellation);
	}

	if (impostor)
	{
		RenderImpostors(vis, renderPass, cmd);
	}

	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);
	device->EventEnd(cmd);

}

void DrawDebugWorld(
	const Scene& scene,
	const CameraComponent& camera,
	const wi::Canvas& canvas,
	CommandList cmd
)
{
	static GPUBuffer wirecubeVB;
	static GPUBuffer wirecubeIB;
	if (!wirecubeVB.IsValid())
	{
		XMFLOAT4 min = XMFLOAT4(-1, -1, -1, 1);
		XMFLOAT4 max = XMFLOAT4(1, 1, 1, 1);

		XMFLOAT4 verts[] = {
			min,							XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,max.y,min.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,max.y,max.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,min.y,max.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,min.y,min.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,max.y,min.z,1),	XMFLOAT4(1,1,1,1),
			max,							XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,min.y,max.z,1),	XMFLOAT4(1,1,1,1),
		};

		GPUBufferDesc bd;
		bd.usage = Usage::DEFAULT;
		bd.size = sizeof(verts);
		bd.bind_flags = BindFlag::VERTEX_BUFFER;
		device->CreateBuffer(&bd, verts, &wirecubeVB);

		uint16_t indices[] = {
			0,1,1,2,0,3,0,4,1,5,4,5,
			5,6,4,7,2,6,3,7,2,3,6,7
		};

		bd.usage = Usage::DEFAULT;
		bd.size = sizeof(indices);
		bd.bind_flags = BindFlag::INDEX_BUFFER;
		device->CreateBuffer(&bd, indices, &wirecubeIB);
	}

	device->EventBegin("DrawDebugWorld", cmd);

	BindCommonResources(cmd);

	if (debugPartitionTree)
	{
		// Actually, there is no SPTree any more, so this will just render all aabbs...
		device->EventBegin("DebugPartitionTree", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;

		for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_objects[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 0, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_lights.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_lights[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 1, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_decals.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_decals[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 0, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_probes.GetCount(); ++i)
		{
			const AABB& aabb = scene.aabb_probes[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(0, 1, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		device->EventEnd(cmd);
	}

	if (debugBoneLines)
	{
		device->EventBegin("DebugBoneLines", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
		{
			const ArmatureComponent& armature = scene.armatures[i];

			if (armature.boneCollection.empty())
			{
				continue;
			}

			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * armature.boneCollection.size(), cmd);

			int j = 0;
			for (Entity entity : armature.boneCollection)
			{
				if (!scene.transforms.Contains(entity))
					continue;
				const TransformComponent& transform = *scene.transforms.GetComponent(entity);
				XMVECTOR a = transform.GetPositionV();
				XMVECTOR b = a + XMVectorSet(0, 1, 0, 0);
				// Search for child to connect bone tip:
				bool child_found = false;
				for (Entity child : armature.boneCollection)
				{
					const HierarchyComponent* hierarchy = scene.hierarchy.GetComponent(child);
					if (hierarchy != nullptr && hierarchy->parentID == entity && scene.transforms.Contains(child))
					{
						const TransformComponent& child_transform = *scene.transforms.GetComponent(child);
						b = child_transform.GetPositionV();
						child_found = true;
						break;
					}
				}
				if (!child_found)
				{
					// No child, try to guess bone tip compared to parent (if it has parent):
					const HierarchyComponent* hierarchy = scene.hierarchy.GetComponent(entity);
					if (hierarchy != nullptr && scene.transforms.Contains(hierarchy->parentID))
					{
						const TransformComponent& parent_transform = *scene.transforms.GetComponent(hierarchy->parentID);
						XMVECTOR ab = a - parent_transform.GetPositionV();
						b = a + ab;
					}
				}

				LineSegment segment;
				XMStoreFloat4(&segment.a, a);
				XMStoreFloat4(&segment.b, b);
				segment.a.w = 1;
				segment.b.w = 1;
				segment.colorA = XMFLOAT4(1, 1, 1, 1);
				segment.colorB = XMFLOAT4(1, 0, 1, 1);

				memcpy((void*)((size_t)mem.data + j * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				j++;
			}

			const GPUBuffer* vbs[] = {
				&mem.buffer
			};
			const uint32_t strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(2 * j, 0, cmd);

		}

		device->EventEnd(cmd);
	}

	if (!renderableTriangles_solid.empty())
	{
		device->EventBegin("DebugTriangles (Solid)", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_SOLID], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		struct Vertex
		{
			XMFLOAT4 position;
			XMFLOAT4 color;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(Vertex) * renderableTriangles_solid.size() * 3, cmd);

		int i = 0;
		for (auto& triangle : renderableTriangles_solid)
		{
			Vertex vertices[3];
			vertices[0].position = XMFLOAT4(triangle.positionA.x, triangle.positionA.y, triangle.positionA.z, 1);
			vertices[0].color = triangle.colorA;
			vertices[1].position = XMFLOAT4(triangle.positionB.x, triangle.positionB.y, triangle.positionB.z, 1);
			vertices[1].color = triangle.colorB;
			vertices[2].position = XMFLOAT4(triangle.positionC.x, triangle.positionC.y, triangle.positionC.z, 1);
			vertices[2].color = triangle.colorC;

			memcpy((void*)((size_t)mem.data + i * sizeof(vertices)), vertices, sizeof(vertices));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(Vertex),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(3 * i, 0, cmd);

		renderableTriangles_solid.clear();

		device->EventEnd(cmd);
	}

	if (!renderableTriangles_wireframe.empty())
	{
		device->EventBegin("DebugTriangles (Wireframe)", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_WIREFRAME], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		struct Vertex
		{
			XMFLOAT4 position;
			XMFLOAT4 color;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(Vertex) * renderableTriangles_wireframe.size() * 3, cmd);

		int i = 0;
		for (auto& triangle : renderableTriangles_wireframe)
		{
			Vertex vertices[3];
			vertices[0].position = XMFLOAT4(triangle.positionA.x, triangle.positionA.y, triangle.positionA.z, 1);
			vertices[0].color = triangle.colorA;
			vertices[1].position = XMFLOAT4(triangle.positionB.x, triangle.positionB.y, triangle.positionB.z, 1);
			vertices[1].color = triangle.colorB;
			vertices[2].position = XMFLOAT4(triangle.positionC.x, triangle.positionC.y, triangle.positionC.z, 1);
			vertices[2].color = triangle.colorC;

			memcpy((void*)((size_t)mem.data + i * sizeof(vertices)), vertices, sizeof(vertices));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(Vertex),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(3 * i, 0, cmd);

		renderableTriangles_wireframe.clear();

		device->EventEnd(cmd);
	}

	if (!renderableLines.empty())
	{
		device->EventBegin("DebugLines", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * renderableLines.size(), cmd);

		int i = 0;
		for (auto& line : renderableLines)
		{
			LineSegment segment;
			segment.a = XMFLOAT4(line.start.x, line.start.y, line.start.z, 1);
			segment.b = XMFLOAT4(line.end.x, line.end.y, line.end.z, 1);
			segment.colorA = line.color_start;
			segment.colorB = line.color_end;

			memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(2 * i, 0, cmd);

		renderableLines.clear();

		device->EventEnd(cmd);
	}

	if (!renderableLines2D.empty())
	{
		device->EventBegin("DebugLines - 2D", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, canvas.GetProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * renderableLines2D.size(), cmd);

		int i = 0;
		for (auto& line : renderableLines2D)
		{
			LineSegment segment;
			segment.a = XMFLOAT4(line.start.x, line.start.y, 0, 1);
			segment.b = XMFLOAT4(line.end.x, line.end.y, 0, 1);
			segment.colorA = line.color_start;
			segment.colorB = line.color_end;

			memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(2 * i, 0, cmd);

		renderableLines2D.clear();

		device->EventEnd(cmd);
	}

	if (!renderablePoints.empty())
	{
		device->EventBegin("DebugPoints", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetProjection()); // only projection, we will expand in view space on CPU below to be camera facing!
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		// Will generate 2 line segments for each point forming a cross section:
		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * renderablePoints.size() * 2, cmd);

		XMMATRIX V = camera.GetView();

		int i = 0;
		for (auto& point : renderablePoints)
		{
			LineSegment segment;
			segment.colorA = segment.colorB = point.color;

			// the cross section will be transformed to view space and expanded here:
			XMVECTOR _c = XMLoadFloat3(&point.position);
			_c = XMVector3Transform(_c, V);

			XMVECTOR _a = _c + XMVectorSet(-1, -1, 0, 0) * point.size;
			XMVECTOR _b = _c + XMVectorSet(1, 1, 0, 0) * point.size;
			XMStoreFloat4(&segment.a, _a);
			XMStoreFloat4(&segment.b, _b);
			memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;

			_a = _c + XMVectorSet(-1, 1, 0, 0) * point.size;
			_b = _c + XMVectorSet(1, -1, 0, 0) * point.size;
			XMStoreFloat4(&segment.a, _a);
			XMStoreFloat4(&segment.b, _b);
			memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(2 * i, 0, cmd);

		renderablePoints.clear();

		device->EventEnd(cmd);
	}

	if (!renderableBoxes.empty())
	{
		device->EventBegin("DebugBoxes", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;

		for (auto& x : renderableBoxes)
		{
			XMStoreFloat4x4(&sb.g_xTransform, XMLoadFloat4x4(&x.first)*camera.GetViewProjection());
			sb.g_xColor = x.second;

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}
		renderableBoxes.clear();

		device->EventEnd(cmd);
	}

	if (!renderableSpheres.empty())
	{
		device->EventBegin("DebugSpheres", cmd);

		static GPUBuffer wiresphereVB;
		static GPUBuffer wiresphereIB;
		if (!wiresphereVB.IsValid())
		{
			struct Vertex
			{
				XMFLOAT4 position;
				XMFLOAT4 color;
			};
			wi::vector<Vertex> vertices;

			const int segmentcount = 36;
			Vertex vert;
			vert.color = XMFLOAT4(1, 1, 1, 1);

			// XY
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(sinf(angle0), cosf(angle0), 0, 1);
				vertices.push_back(vert);
			}
			// XZ
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(sinf(angle0), 0, cosf(angle0), 1);
				vertices.push_back(vert);
			}
			// YZ
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(0, sinf(angle0), cosf(angle0), 1);
				vertices.push_back(vert);
			}

			GPUBufferDesc bd;
			bd.usage = Usage::DEFAULT;
			bd.size = vertices.size() * sizeof(Vertex);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, vertices.data(), &wiresphereVB);

			wi::vector<uint16_t> indices;
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i));
				indices.push_back(uint16_t(i + 1));
			}
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i + segmentcount + 1));
				indices.push_back(uint16_t(i + segmentcount + 1 + 1));
			}
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i + (segmentcount + 1) * 2));
				indices.push_back(uint16_t(i + (segmentcount + 1) * 2 + 1));
			}

			bd.usage = Usage::DEFAULT;
			bd.size = indices.size() * sizeof(uint16_t);
			bd.bind_flags = BindFlag::INDEX_BUFFER;
			device->CreateBuffer(&bd, indices.data(), &wiresphereIB);
		}

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		const GPUBuffer* vbs[] = {
			&wiresphereVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wiresphereIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;

		for (auto& x : renderableSpheres)
		{
			const Sphere& sphere = x.first;
			XMStoreFloat4x4(&sb.g_xTransform,
				XMMatrixScaling(sphere.radius, sphere.radius, sphere.radius) *
				XMMatrixTranslation(sphere.center.x, sphere.center.y, sphere.center.z) *
				camera.GetViewProjection()
			);
			sb.g_xColor = x.second;

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed((uint32_t)(wiresphereIB.GetDesc().size / sizeof(uint16_t)), 0, 0, cmd);
		}
		renderableSpheres.clear();

		device->EventEnd(cmd);
	}

	if (!renderableCapsules.empty())
	{
		device->EventBegin("DebugCapsules", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		const int segmentcount = 18 + 1 + 18 + 1;
		const int linecount = (int)renderableCapsules.size() * segmentcount;

		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * (size_t)linecount, cmd);

		XMVECTOR Eye = camera.GetEye();
		XMVECTOR Unit = XMVectorSet(0, 1, 0, 0);

		LineSegment* linearray = (LineSegment*)mem.data;
		int j = 0;
		for (auto& it : renderableCapsules)
		{
			const Capsule& capsule = it.first;
			const float radius = capsule.radius;

			XMVECTOR Base = XMLoadFloat3(&capsule.base);
			XMVECTOR Tip = XMLoadFloat3(&capsule.tip);
			XMVECTOR Radius = XMVectorReplicate(capsule.radius);
			XMVECTOR Normal = XMVector3Normalize(Tip - Base);
			XMVECTOR Tangent = XMVector3Normalize(XMVector3Cross(Normal, Base - Eye));
			XMVECTOR Binormal = XMVector3Normalize(XMVector3Cross(Tangent, Normal));
			XMVECTOR LineEndOffset = Normal * Radius;
			XMVECTOR A = Base + LineEndOffset;
			XMVECTOR B = Tip - LineEndOffset;
			XMVECTOR AB = Unit * XMVector3Length(B - A);
			XMMATRIX M = { Tangent,Normal,Binormal,XMVectorSetW(A, 1) };

			for (int i = 0; i < segmentcount; i += 1)
			{
				const float angle0 = XM_PIDIV2 + (float)i / (float)segmentcount * XM_2PI;
				const float angle1 = XM_PIDIV2 + (float)(i + 1) / (float)segmentcount * XM_2PI;
				XMVECTOR a, b;
				if (i < 18)
				{

					a = XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
					b = XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
				}
				else if (i == 18)
				{
					a = XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
					b = AB + XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
				}
				else if (i > 18 && i < 18 + 1 + 18)
				{
					a = AB + XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
					b = AB + XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
				}
				else
				{
					a = AB + XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
					b = XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
				}
				a = XMVector3Transform(a, M);
				b = XMVector3Transform(b, M);
				LineSegment line;
				XMStoreFloat4(&line.a, a);
				XMStoreFloat4(&line.b, b);
				line.colorA = line.colorB = it.second;
				std::memcpy(linearray + j, &line, sizeof(line));
				j++;
			}

		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(linecount * 2, 0, cmd);

		renderableCapsules.clear();

		device->EventEnd(cmd);
	}


	if (debugEnvProbes && scene.envmapArray.IsValid())
	{
		device->EventBegin("Debug EnvProbes", cmd);
		// Envmap spheres:

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_ENVPROBE], cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = scene.probes[i];

			XMStoreFloat4x4(&sb.g_xTransform, XMMatrixTranslationFromVector(XMLoadFloat3(&probe.position)));
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			if (probe.textureIndex < 0)
			{
				device->BindResource(wi::texturehelper::getBlackCubeMap(), 0, cmd);
			}
			else
			{
				device->BindResource(&scene.envmapArray, 0, cmd, scene.envmapArray.GetDesc().mip_levels + probe.textureIndex);
			}

			device->Draw(2880, 0, cmd); // uv-sphere
		}


		// Local proxy boxes:

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = scene.probes[i];
			Entity entity = scene.probes.GetEntity(i);

			if (probe.textureIndex < 0 || !scene.transforms.Contains(entity))
			{
				continue;
			}

			const TransformComponent& transform = *scene.transforms.GetComponent(entity);

			XMStoreFloat4x4(&sb.g_xTransform, XMLoadFloat4x4(&transform.world)*camera.GetViewProjection());
			sb.g_xColor = float4(0, 1, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		device->EventEnd(cmd);
	}


	if (GetDDGIDebugEnabled() && GetDDGIEnabled())
	{
		device->EventBegin("Debug DDGI", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_DDGI], cmd);
		device->DrawInstanced(2880, DDGI_GRID_DIMENSIONS.x * DDGI_GRID_DIMENSIONS.y * DDGI_GRID_DIMENSIONS.z, 0, 0, cmd); // uv-sphere

		device->EventEnd(cmd);
	}


	if (gridHelper)
	{
		device->EventBegin("GridHelper", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_GRID], cmd);

		static float col = 0.7f;
		static uint32_t gridVertexCount = 0;
		static GPUBuffer grid;
		if (!grid.IsValid())
		{
			const float h = 0.01f; // avoid z-fight with zero plane
			const int a = 20;
			XMFLOAT4 verts[((a + 1) * 2 + (a + 1) * 2) * 2];

			int count = 0;
			for (int i = 0; i <= a; ++i)
			{
				verts[count++] = XMFLOAT4(i - a * 0.5f, h, -a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(0, 0, 1, 1) : XMFLOAT4(col, col, col, 1));

				verts[count++] = XMFLOAT4(i - a * 0.5f, h, +a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(0, 0, 1, 1) : XMFLOAT4(col, col, col, 1));
			}
			for (int j = 0; j <= a; ++j)
			{
				verts[count++] = XMFLOAT4(-a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, 0, 0, 1) : XMFLOAT4(col, col, col, 1));

				verts[count++] = XMFLOAT4(+a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, 0, 0, 1) : XMFLOAT4(col, col, col, 1));
			}

			gridVertexCount = arraysize(verts) / 2;

			GPUBufferDesc bd;
			bd.size = sizeof(verts);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, verts, &grid);
		}

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = float4(1, 1, 1, 1);

		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		const GPUBuffer* vbs[] = {
			&grid,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->Draw(gridVertexCount, 0, cmd);

		device->EventEnd(cmd);
	}

	if (voxelHelper && textures[TEXTYPE_3D_VOXELRADIANCE].IsValid())
	{
		device->EventBegin("Debug Voxels", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_VOXEL], cmd);


		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, XMMatrixTranslationFromVector(XMLoadFloat3(&voxelSceneData.center)) * camera.GetViewProjection());
		sb.g_xColor = float4(1, 1, 1, 1);

		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		device->Draw(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res, 0, cmd);

		device->EventEnd(cmd);
	}

	if (debugEmitters)
	{
		device->EventBegin("DebugEmitters", cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
		{
			const wi::EmittedParticleSystem& emitter = scene.emitters[i];
			Entity entity = scene.emitters.GetEntity(i);
			if (!scene.transforms.Contains(entity))
			{
				continue;
			}
			const TransformComponent& transform = *scene.transforms.GetComponent(entity);
			const MeshComponent* mesh = scene.meshes.GetComponent(emitter.meshID);

			XMStoreFloat4x4(&sb.g_xTransform, XMLoadFloat4x4(&transform.world)*camera.GetViewProjection());
			sb.g_xColor = float4(0, 1, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			if (mesh == nullptr)
			{
				// No mesh, just draw a box:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);
				const GPUBuffer* vbs[] = {
					&wirecubeVB,
				};
				const uint32_t strides[] = {
					sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
				device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);
				device->DrawIndexed(24, 0, 0, cmd);
			}
			else
			{
				// Draw mesh wireframe:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_EMITTER], cmd);
				const GPUBuffer* vbs[] = {
					mesh->streamoutBuffer.IsValid() ? &mesh->streamoutBuffer : &mesh->generalBuffer,
				};
				const uint32_t strides[] = {
					sizeof(MeshComponent::Vertex_POS),
				};
				const uint64_t offsets[] = {
					mesh->so_pos_nor_wind.IsValid() ? mesh->so_pos_nor_wind.offset : mesh->vb_pos_nor_wind.offset,
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);
				device->BindIndexBuffer(&mesh->generalBuffer, mesh->GetIndexFormat(), mesh->ib.offset, cmd);

				device->DrawIndexed((uint32_t)mesh->indices.size(), 0, 0, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (!paintrads.empty())
	{
		device->EventBegin("Paint Radiuses", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_PAINTRADIUS], cmd);

		for (auto& x : paintrads)
		{
			if (!scene.transforms.Contains(x.objectEntity) ||
				!scene.objects.Contains(x.objectEntity)
				)
			{
				continue;
			}
			const ObjectComponent& object = *scene.objects.GetComponent(x.objectEntity);
			const TransformComponent& transform = *scene.transforms.GetComponent(x.objectEntity);
			if (scene.meshes.GetCount() < object.mesh_index)
			{
				continue;
			}
			const MeshComponent& mesh = scene.meshes[object.mesh_index];
			const MeshComponent::MeshSubset& subset = mesh.subsets[x.subset];
			if (!scene.materials.Contains(subset.materialID))
			{
				continue;
			}
			const MaterialComponent& material = *scene.materials.GetComponent(subset.materialID);

			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(ShaderMeshInstancePointer), cmd);
			ShaderMeshInstancePointer poi;
			poi.Create((uint)scene.objects.GetIndex(x.objectEntity));
			std::memcpy(mem.data, &poi, sizeof(poi));

			device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

			PaintRadiusCB cb;
			cb.xPaintRadResolution = x.dimensions;
			cb.xPaintRadCenter = x.center;
			cb.xPaintRadRadius = x.radius;
			cb.xPaintRadUVSET = x.uvset;
			cb.xPaintRadBrushRotation = x.rotation;
			cb.xPaintRadBrushShape = x.shape;
			device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(PaintRadiusCB), cmd);

			ObjectPushConstants push;
			push.geometryIndex = mesh.geometryOffset + x.subset;
			push.materialIndex = subset.materialIndex;
			push.instances = device->GetDescriptorIndex(&mem.buffer, SubresourceType::SRV);
			push.instance_offset = (uint)mem.offset;
			device->PushConstants(&push, sizeof(push), cmd);

			device->DrawIndexedInstanced(subset.indexCount, 1, subset.indexOffset, 0, 0, cmd);
		}

		paintrads.clear();

		device->EventEnd(cmd);
	}


	if (debugForceFields)
	{
		device->EventBegin("DebugForceFields", cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.forces.GetCount(); ++i)
		{
			const ForceFieldComponent& force = scene.forces[i];

			XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(camera.Eye.x, camera.Eye.y, camera.Eye.z, (float)i);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			switch (force.type)
			{
			case ENTITY_TYPE_FORCEFIELD_POINT:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_FORCEFIELD_POINT], cmd);
				device->Draw(2880, 0, cmd); // uv-sphere
				break;
			case ENTITY_TYPE_FORCEFIELD_PLANE:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_FORCEFIELD_PLANE], cmd);
				device->Draw(14, 0, cmd); // box
				break;
			}
		}

		device->EventEnd(cmd);
	}


	if (debugCameras)
	{
		device->EventBegin("DebugCameras", cmd);

		static GPUBuffer wirecamVB;
		static GPUBuffer wirecamIB;
		if (!wirecamVB.IsValid())
		{
			XMFLOAT4 verts[] = {
				XMFLOAT4(-0.1f,-0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0.1f,-0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0.1f,0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-0.1f,0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-1,-1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(1,-1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(1,1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-1,1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0,1.5f,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0,0,-1,1),	XMFLOAT4(1,1,1,1),
			};

			GPUBufferDesc bd;
			bd.usage = Usage::DEFAULT;
			bd.size = sizeof(verts);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, verts, &wirecamVB);

			uint16_t indices[] = {
				0,1,1,2,0,3,0,4,1,5,4,5,
				5,6,4,7,2,6,3,7,2,3,6,7,
				6,8,7,8,
				0,2,1,3
			};

			bd.usage = Usage::DEFAULT;
			bd.size = sizeof(indices);
			bd.bind_flags = BindFlag::INDEX_BUFFER;
			device->CreateBuffer(&bd, indices, &wirecamIB);
		}

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);

		const GPUBuffer* vbs[] = {
			&wirecamVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecamIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);

		for (size_t i = 0; i < scene.cameras.GetCount(); ++i)
		{
			const CameraComponent& cam = scene.cameras[i];
			Entity entity = scene.cameras.GetEntity(i);

			const float aspect = cam.width / cam.height;
			XMStoreFloat4x4(&sb.g_xTransform, XMMatrixScaling(aspect * 0.5f, 0.5f, 0.5f) * cam.GetInvView()*camera.GetViewProjection());

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(32, 0, 0, cmd);
		}

		device->EventEnd(cmd);
	}

	if (GetRaytraceDebugBVHVisualizerEnabled())
	{
		//PE: Check if debug BVH is possible. or it will crash.
		if (GetSurfelGIEnabled() || GetDDGIEnabled() )
		{
			RayTraceSceneBVH(scene, cmd);
		}
	}

	if (!debugTextStorage.empty())
	{
		device->EventBegin("DebugTexts", cmd);
		const XMMATRIX VP = camera.GetViewProjection();
		const XMMATRIX R = XMLoadFloat3x3(&camera.rotationMatrix);
		struct DebugTextSorter
		{
			const char* text;
			size_t text_len;
			DebugTextParams params;
			float distance;
		};
		static thread_local wi::vector<DebugTextSorter> sorted_texts;
		sorted_texts.clear();
		size_t offset = 0;
		while(offset < debugTextStorage.size())
		{
			auto& x = sorted_texts.emplace_back();
			x.params = *(const DebugTextParams*)(debugTextStorage.data() + offset);
			offset += sizeof(DebugTextParams);
			x.text = (const char*)(debugTextStorage.data() + offset);
			x.text_len = strlen(x.text);
			offset += x.text_len + 1;
			x.distance = wi::math::Distance(x.params.position, camera.Eye);

		}
		std::sort(sorted_texts.begin(), sorted_texts.end(), [](const DebugTextSorter& a, const DebugTextSorter& b) {
			return a.distance > b.distance;
			});
		for (auto& x : sorted_texts)
		{
			wi::font::Params params;
			params.position = x.params.position;
			params.size = x.params.pixel_height;
			params.scaling = 1.0f / params.size * x.params.scaling;
			params.color = wi::Color::fromFloat4(x.params.color);
			params.h_align = wi::font::WIFALIGN_CENTER;
			params.v_align = wi::font::WIFALIGN_CENTER;
			params.softness = 0.1f;
			params.shadowColor = wi::Color::Black();
			params.shadow_softness = 0.8f;
			params.customProjection = &VP;
			if (x.params.flags & DebugTextParams::DEPTH_TEST)
			{
				params.enableDepthTest();
			}
			if (x.params.flags & DebugTextParams::CAMERA_FACING)
			{
				params.customRotation = &R;
			}
			if (x.params.flags & DebugTextParams::CAMERA_SCALING)
			{
				params.scaling *= x.distance * 0.05f;
			}
			wi::font::Draw(x.text, x.text_len, params, cmd);
		}
		debugTextStorage.clear();
		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}


void RenderAtmosphericScatteringTextures(CommandList cmd)
{
	device->EventBegin("ComputeAtmosphericScatteringTextures", cmd);
	auto range = wi::profiler::BeginRangeGPU("Atmospheric Scattering Textures", cmd);

	// Transmittance Lut pass:
	{
		device->EventBegin("TransmittanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_TRANSMITTANCELUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd); // empty
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd); // empty

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int threadSize = 8;
		const int transmittanceLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].GetDesc().width;
		const int transmittanceLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].GetDesc().height;
		const int transmittanceLutThreadX = static_cast<uint32_t>(std::ceil(transmittanceLutWidth / threadSize));
		const int transmittanceLutThreadY = static_cast<uint32_t>(std::ceil(transmittanceLutHeight / threadSize));

		device->Dispatch(transmittanceLutThreadX, transmittanceLutThreadY, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// MultiScattered Luminance Lut pass:
	{
		device->EventBegin("MultiScatteredLuminanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], cmd);

		// Use transmittance from previous pass
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int multiScatteredLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].GetDesc().width;
		const int multiScatteredLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].GetDesc().height;

		device->Dispatch(multiScatteredLutWidth, multiScatteredLutHeight, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Environment Luminance Lut pass:
	{
		device->EventBegin("EnvironmentLuminanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_SKYLUMINANCELUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int environmentLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].GetDesc().width;
		const int environmentLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].GetDesc().height;

		device->Dispatch(environmentLutWidth, environmentLutHeight, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);

	RefreshAtmosphericScatteringTextures(cmd);

	wi::profiler::EndRange(range);
}
void RefreshAtmosphericScatteringTextures(CommandList cmd)
{
	device->EventBegin("UpdateAtmosphericScatteringTextures", cmd);

	// Sky View Lut pass:
	{
		device->EventBegin("SkyViewLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_SKYVIEWLUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int threadSize = 8;
		const int skyViewLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].GetDesc().width;
		const int skyViewLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].GetDesc().height;
		const int skyViewLutThreadX = static_cast<uint32_t>(std::ceil(skyViewLutWidth / threadSize));
		const int skyViewLutThreadY = static_cast<uint32_t>(std::ceil(skyViewLutHeight / threadSize));

		device->Dispatch(skyViewLutThreadX, skyViewLutThreadY, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}
void DrawSky(const Scene& scene, CommandList cmd)
{
	device->EventBegin("DrawSky", cmd);
	
	if (scene.weather.skyMap.IsValid())
	{
		device->BindPipelineState(&PSO_sky[SKYRENDERING_STATIC], cmd);
	}
	else
	{
		device->BindPipelineState(&PSO_sky[SKYRENDERING_DYNAMIC], cmd);
	}

	BindCommonResources(cmd);

	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}
void DrawSun(CommandList cmd)
{
	device->EventBegin("DrawSun", cmd);

	device->BindPipelineState(&PSO_sky[SKYRENDERING_SUN], cmd);

	BindCommonResources(cmd);

	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}


void RefreshEnvProbes(const Visibility& vis, CommandList cmd)
{
	if (!vis.scene->envmapArray.IsValid())
		return;

	device->EventBegin("EnvironmentProbe Refresh", cmd);
	auto range = wi::profiler::BeginRangeGPU("Environment Probe Refresh", cmd);

	BindCommonResources(cmd);

	Viewport vp;
	vp.height = vp.width = (float)vis.scene->envmapArray.desc.width;
	device->BindViewports(1, &vp, cmd);

	const float zNearP = vis.camera->zNearP;
	const float zFarP = vis.camera->zFarP;

	auto render_probe = [&](const EnvironmentProbeComponent& probe, const AABB& probe_aabb) {

		SHCAM cameras[6];
		CreateCubemapCameras(probe.position, zNearP, zFarP, cameras, arraysize(cameras));
		Frustum frusta[arraysize(cameras)];

		CubemapRenderCB cb;
		for (uint32_t i = 0; i < arraysize(cameras); ++i)
		{
			frusta[i] = cameras[i].frustum;
			XMStoreFloat4x4(&cb.xCubemapRenderCams[i].view_projection, cameras[i].view_projection);
			cb.xCubemapRenderCams[i].properties = uint4(i, 0, 0, 0);
		}
		device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(CubemapRenderCB), cmd);

		CameraCB camcb;
		camcb.position = probe.position; // only this will be used by envprobe rendering shaders the rest is read from cubemaprenderCB
		device->BindDynamicConstantBuffer(camcb, CBSLOT_RENDERER_CAMERA, cmd);

		if (vis.scene->weather.IsRealisticSky())
		{
			// Refresh atmospheric textures, since each probe has different positions
			RefreshAtmosphericScatteringTextures(cmd);
		}

		if (probe.IsMSAA())
		{
			device->RenderPassBegin(&vis.scene->renderpasses_envmap_MSAA[probe.textureIndex], cmd);
		}
		else
		{
			device->RenderPassBegin(&vis.scene->renderpasses_envmap[probe.textureIndex], cmd);
		}

		// Scene will only be rendered if this is a real probe entity:
		if (probe_aabb.layerMask & vis.layerMask)
		{
			Sphere culler(probe.position, zFarP);

			static thread_local RenderQueue renderQueue;
			renderQueue.init();
			for (size_t i = 0; i < vis.scene->aabb_objects.GetCount(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_objects[i];
				if ((aabb.layerMask & vis.layerMask) && (aabb.layerMask & probe_aabb.layerMask) && culler.intersects(aabb))
				{
					const ObjectComponent& object = vis.scene->objects[i];
					if (object.IsRenderable())
					{
						renderQueue.add(object.mesh_index, uint32_t(i), 0);
					}
				}
			}

			if (!renderQueue.empty())
			{
				RenderMeshes(vis, renderQueue, RENDERPASS_ENVMAPCAPTURE, RENDERTYPE_ALL, cmd, false, frusta, arraysize(frusta));
			}
		}

		// sky
		{
			if (vis.scene->weather.skyMap.IsValid())
			{
				device->BindPipelineState(&PSO_sky[SKYRENDERING_ENVMAPCAPTURE_STATIC], cmd);
				device->BindResource(&vis.scene->weather.skyMap.GetTexture(), 0, cmd);
			}
			else
			{
				device->BindPipelineState(&PSO_sky[SKYRENDERING_ENVMAPCAPTURE_DYNAMIC], cmd);
			}

			device->DrawInstanced(240, 6, 0, 0, cmd); // 6 instances so it will be replicated for every cubemap face
		}

		device->RenderPassEnd(cmd);

		MIPGEN_OPTIONS mipopt;
		mipopt.arrayIndex = probe.textureIndex;
		GenerateMipChain(vis.scene->envmapArray, MIPGENFILTER_LINEAR, cmd, mipopt);

		// Filter the enviroment map mip chain according to BRDF:
		//	A bit similar to MIP chain generation, but its input is the MIP-mapped texture,
		//	and we generatethe filtered MIPs from bottom to top.
		device->EventBegin("FilterEnvMap", cmd);
		{
			TextureDesc desc = vis.scene->envmapArray.GetDesc();
			int arrayIndex = probe.textureIndex;

			device->BindComputeShader(&shaders[CSTYPE_FILTERENVMAP], cmd);

			desc.width = 1;
			desc.height = 1;
			for (uint32_t i = desc.mip_levels - 1; i > 0; --i)
			{
				{
					GPUBarrier barriers[] = {
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 0),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 1),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 2),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 3),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 4),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS, i, arrayIndex * 6 + 5),
					};
					device->Barrier(barriers, arraysize(barriers), cmd);
				}

				device->BindUAV(&vis.scene->envmapArray, 0, cmd, i);
				device->BindResource(&vis.scene->envmapArray, 0, cmd, std::max(0, (int)i - 2));

				FilterEnvmapPushConstants push;
				push.filterResolution.x = desc.width;
				push.filterResolution.y = desc.height;
				push.filterResolution_rcp.x = 1.0f / push.filterResolution.x;
				push.filterResolution_rcp.y = 1.0f / push.filterResolution.y;
				push.filterArrayIndex = arrayIndex;
				push.filterRoughness = (float)i / (float)desc.mip_levels;
				push.filterRayCount = 128;
				push.texture_input = device->GetDescriptorIndex(&vis.scene->envmapArray, SubresourceType::SRV, std::max(0, (int)i - 2));
				push.texture_output = device->GetDescriptorIndex(&vis.scene->envmapArray, SubresourceType::UAV, i);
				device->PushConstants(&push, sizeof(push), cmd);

				device->Dispatch(
					(desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE,
					(desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE,
					6,
					cmd);

				{
					GPUBarrier barriers[] = {
						GPUBarrier::Memory(),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 0),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 1),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 2),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 3),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 4),
						GPUBarrier::Image(&vis.scene->envmapArray, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE, i, arrayIndex * 6 + 5),
					};
					device->Barrier(barriers, arraysize(barriers), cmd);
				}

				desc.width *= 2;
				desc.height *= 2;
			}
		}
		device->EventEnd(cmd);
	};

	if (vis.scene->probes.GetCount() == 0)
	{
		// In this case, there are no probes, so the sky will be rendered to first envmap:
		EnvironmentProbeComponent probe;
		probe.textureIndex = 0;
		probe.position = vis.camera->Eye;

		AABB probe_aabb;
		probe_aabb.layerMask = 0;
		render_probe(probe, probe_aabb);
	}
	else
	{
		for (size_t i = 0; i < vis.scene->probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = vis.scene->probes[i];
			const AABB& probe_aabb = vis.scene->aabb_probes[i];

			if ((probe_aabb.layerMask & vis.layerMask) && probe.render_dirty && probe.textureIndex >= 0 && probe.textureIndex < vis.scene->envmapCount)
			{
				probe.render_dirty = false;
				render_probe(probe, probe_aabb);
			}
		}
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd); // EnvironmentProbe Refresh
}

void RefreshImpostors(const Scene& scene, CommandList cmd)
{
	if (!scene.impostorArray.IsValid())
		return;

	device->EventBegin("Impostor Refresh", cmd);

	device->BindPipelineState(&PSO_captureimpostor, cmd);

	Viewport viewport;
	viewport.width = viewport.height = (float)scene.impostorTextureDim;
	device->BindViewports(1, &viewport, cmd);

	BindCommonResources(cmd);

	barrier_stack.push_back(GPUBarrier::Image(&scene.impostorArray, ResourceState::SHADER_RESOURCE, ResourceState::RENDERTARGET));
	barrier_stack_flush(cmd);

	for (uint32_t impostorIndex = 0; impostorIndex < scene.impostors.GetCount(); ++impostorIndex)
	{
		const ImpostorComponent& impostor = scene.impostors[impostorIndex];
		if (!impostor.render_dirty)
			continue;
		impostor.render_dirty = false;

		Entity entity = scene.impostors.GetEntity(impostorIndex);
		const MeshComponent& mesh = *scene.meshes.GetComponent(entity);

		// impostor camera will fit around mesh bounding sphere:
		const Sphere boundingsphere = mesh.GetBoundingSphere();

		device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

		for (size_t i = 0; i < impostorCaptureAngles; ++i)
		{
			CameraComponent impostorcamera;
			impostorcamera.SetCustomProjectionEnabled(true);
			TransformComponent camera_transform;

			camera_transform.ClearTransform();
			camera_transform.Translate(boundingsphere.center);

			XMMATRIX P = XMMatrixOrthographicOffCenterLH(-boundingsphere.radius, boundingsphere.radius, -boundingsphere.radius, boundingsphere.radius, -boundingsphere.radius, boundingsphere.radius);
			XMStoreFloat4x4(&impostorcamera.Projection, P);
			XMVECTOR Q = XMQuaternionNormalize(XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XM_2PI * (float)i / (float)impostorCaptureAngles));
			XMStoreFloat4(&camera_transform.rotation_local, Q);

			camera_transform.UpdateTransform();
			impostorcamera.TransformCamera(camera_transform);
			impostorcamera.UpdateCamera();

			BindCameraCB(impostorcamera, impostorcamera, impostorcamera, cmd);

			int slice = (int)(impostor.textureIndex * impostorCaptureAngles + i);
			device->RenderPassBegin(&scene.renderpasses_impostor[slice], cmd);

			uint32_t first_subset = 0;
			uint32_t last_subset = 0;
			mesh.GetLODSubsetRange(0, first_subset, last_subset);
			for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
			{
				const MeshComponent::MeshSubset& subset = mesh.subsets[subsetIndex];
				if (subset.indexCount == 0)
				{
					continue;
				}
				const MaterialComponent& material = *scene.materials.GetComponent(subset.materialID);

				ObjectPushConstants push;
				push.geometryIndex = mesh.geometryOffset + subsetIndex;
				push.materialIndex = subset.materialIndex;
				push.instances = -1;
				push.instance_offset = 0;
				device->PushConstants(&push, sizeof(push), cmd);

				device->DrawIndexedInstanced(subset.indexCount, 1, subset.indexOffset, 0, 0, cmd);
			}

			device->RenderPassEnd(cmd);
		}

	}

	barrier_stack.push_back(GPUBarrier::Image(&scene.impostorArray, ResourceState::RENDERTARGET, ResourceState::SHADER_RESOURCE));
	barrier_stack_flush(cmd);

	device->EventEnd(cmd);
}

void VoxelRadiance(const Visibility& vis, CommandList cmd)
{
	if (!GetVoxelRadianceEnabled())
	{
		return;
	}

	device->EventBegin("Voxel Radiance", cmd);
	auto range = wi::profiler::BeginRangeGPU("Voxel Radiance", cmd);

	static RenderPass renderpass_voxelize;

	if (!renderpass_voxelize.IsValid())
	{
		RenderPassDesc renderpassdesc;
		renderpassdesc.flags = RenderPassDesc::Flags::ALLOW_UAV_WRITES;
		device->CreateRenderPass(&renderpassdesc, &renderpass_voxelize);
	}

	Texture* result = &textures[TEXTYPE_3D_VOXELRADIANCE];

	AABB bbox;
	XMFLOAT3 extents = voxelSceneData.extents;
	XMFLOAT3 center = voxelSceneData.center;
	bbox.createFromHalfWidth(center, extents);


	static thread_local RenderQueue renderQueue;
	renderQueue.init();
	for (size_t i = 0; i < vis.scene->aabb_objects.GetCount(); ++i)
	{
		const AABB& aabb = vis.scene->aabb_objects[i];
		if (bbox.intersects(aabb))
		{
			const ObjectComponent& object = vis.scene->objects[i];
			if (object.IsRenderable())
			{
				renderQueue.add(object.mesh_index, uint32_t(i), 0);
			}
		}
	}

	if (!renderQueue.empty())
	{
		Viewport vp;
		vp.width = (float)voxelSceneData.res;
		vp.height = (float)voxelSceneData.res;
		device->BindViewports(1, &vp, cmd);

		GPUResource* UAVs[] = { &resourceBuffers[RBTYPE_VOXELSCENE] };
		device->BindUAVs(UAVs, 0, 1, cmd);

		BindCommonResources(cmd);


		device->RenderPassBegin(&renderpass_voxelize, cmd);
		RenderMeshes(vis, renderQueue, RENDERPASS_VOXELIZE, RENDERTYPE_OPAQUE, cmd, false, nullptr, 1);
		device->RenderPassEnd(cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		// Copy the packed voxel scene data to a 3D texture, then delete the voxel scene emission data. The cone tracing will operate on the 3D texture
		device->EventBegin("Voxel Scene Copy - Clear", cmd);
		device->BindUAV(&resourceBuffers[RBTYPE_VOXELSCENE], 0, cmd);
		device->BindUAV(&textures[TEXTYPE_3D_VOXELRADIANCE], 1, cmd);

		static bool smooth_copy = true;
		if (smooth_copy)
		{
			device->BindComputeShader(&shaders[CSTYPE_VOXELSCENECOPYCLEAR_TEMPORALSMOOTHING], cmd);
		}
		else
		{
			device->BindComputeShader(&shaders[CSTYPE_VOXELSCENECOPYCLEAR], cmd);
		}
		device->Dispatch((uint32_t)(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res / 256), 1, 1, cmd);
		device->EventEnd(cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		if (voxelSceneData.secondaryBounceEnabled)
		{
			device->EventBegin("Voxel Radiance Secondary Bounce", cmd);
			// Pre-integrate the voxel texture by creating blurred mip levels:
			GenerateMipChain(textures[TEXTYPE_3D_VOXELRADIANCE], MIPGENFILTER_LINEAR, cmd);

			device->BindResource(&textures[TEXTYPE_3D_VOXELRADIANCE], 0, cmd);
			device->BindResource(&resourceBuffers[RBTYPE_VOXELSCENE], 1, cmd);
			device->BindUAV(&textures[TEXTYPE_3D_VOXELRADIANCE_HELPER], 0, cmd);
			device->BindComputeShader(&shaders[CSTYPE_VOXELRADIANCESECONDARYBOUNCE], cmd);
			device->Dispatch(voxelSceneData.res / 8, voxelSceneData.res / 8, voxelSceneData.res / 8, cmd);
			device->EventEnd(cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventBegin("Voxel Scene Clear Normals", cmd);
			device->BindUAV(&resourceBuffers[RBTYPE_VOXELSCENE], 0, cmd);
			device->BindComputeShader(&shaders[CSTYPE_VOXELCLEARONLYNORMAL], cmd);
			device->Dispatch((uint32_t)(voxelSceneData.res * voxelSceneData.res * voxelSceneData.res / 256), 1, 1, cmd);
			device->EventEnd(cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			result = &textures[TEXTYPE_3D_VOXELRADIANCE_HELPER];
		}



		// Pre-integrate the voxel texture by creating blurred mip levels:
		{
			GenerateMipChain(*result, MIPGENFILTER_LINEAR, cmd);
		}
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}


void CreateTiledLightResources(TiledLightResources& res, XMUINT2 resolution)
{
	res.tileCount = GetEntityCullingTileCount(resolution);

	{
		GPUBufferDesc bd;
		bd.stride = sizeof(XMFLOAT4) * 4; // storing 4 planes for every tile
		bd.size = bd.stride * res.tileCount.x * res.tileCount.y;
		bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
		bd.usage = Usage::DEFAULT;
		device->CreateBuffer(&bd, nullptr, &res.tileFrustums);

		device->SetName(&res.tileFrustums, "tileFrustums");
	}
	{
		GPUBufferDesc bd;
		bd.stride = sizeof(uint);
		bd.size = res.tileCount.x * res.tileCount.y * bd.stride * SHADER_ENTITY_TILE_BUCKET_COUNT;
		bd.usage = Usage::DEFAULT;
		bd.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
		bd.misc_flags = ResourceMiscFlag::BUFFER_RAW;
		device->CreateBuffer(&bd, nullptr, &res.entityTiles_Opaque);
		device->CreateBuffer(&bd, nullptr, &res.entityTiles_Transparent);

		device->SetName(&res.entityTiles_Opaque, "entityTiles_Opaque");
		device->SetName(&res.entityTiles_Transparent, "entityTiles_Transparent");
	}
}
void ComputeTiledLightCulling(
	const TiledLightResources& res,
	const Texture& debugUAV,
	CommandList cmd
)
{
	BindCommonResources(cmd);
	auto range = wi::profiler::BeginRangeGPU("Entity Culling", cmd);

	// Frustum computation
	{
		device->EventBegin("Tile Frustums", cmd);
		device->BindComputeShader(&shaders[CSTYPE_TILEFRUSTUMS], cmd);

		const GPUResource* uavs[] = { 
			&res.tileFrustums 
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.tileFrustums, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.tileCount.x + TILED_CULLING_BLOCKSIZE - 1) / TILED_CULLING_BLOCKSIZE,
			(res.tileCount.y + TILED_CULLING_BLOCKSIZE - 1) / TILED_CULLING_BLOCKSIZE,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	// Perform the culling
	{
		device->EventBegin("Entity Culling", cmd);

		device->BindResource(&res.tileFrustums, 0, cmd);

		if (GetDebugLightCulling() && debugUAV.IsValid())
		{
			device->BindComputeShader(&shaders[GetAdvancedLightCulling() ? CSTYPE_LIGHTCULLING_ADVANCED_DEBUG : CSTYPE_LIGHTCULLING_DEBUG], cmd);
			device->BindUAV(&debugUAV, 3, cmd);
		}
		else
		{
			device->BindComputeShader(&shaders[GetAdvancedLightCulling() ? CSTYPE_LIGHTCULLING_ADVANCED : CSTYPE_LIGHTCULLING], cmd);
		}

		const GPUResource* uavs[] = {
			&res.entityTiles_Transparent,
			&res.entityTiles_Opaque,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.tileFrustums, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.entityTiles_Transparent, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Buffer(&res.entityTiles_Opaque, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(res.tileCount.x, res.tileCount.y, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.entityTiles_Opaque, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.entityTiles_Transparent, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Unbind from UAV slots:
	GPUResource empty;
	const GPUResource* uavs[] = {
		&empty,
		&empty
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	wi::profiler::EndRange(range);
}


void ResolveMSAADepthBuffer(const Texture& dst, const Texture& src, CommandList cmd)
{
	device->EventBegin("ResolveMSAADepthBuffer", cmd);

	device->BindResource(&src, 0, cmd);
	device->BindUAV(&dst, 0, cmd);

	const TextureDesc& desc = src.GetDesc();

	device->BindComputeShader(&shaders[CSTYPE_RESOLVEMSAADEPTHSTENCIL], cmd);
	device->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1, cmd);



	device->EventEnd(cmd);
}
void DownsampleDepthBuffer(const Texture& src, CommandList cmd)
{
	device->EventBegin("DownsampleDepthBuffer", cmd);

	device->BindPipelineState(&PSO_downsampledepthbuffer, cmd);

	device->BindResource(&src, 0, cmd);

	device->Draw(3, 0, cmd);


	device->EventEnd(cmd);
}

void GenerateMipChain(const Texture& texture, MIPGENFILTER filter, CommandList cmd, const MIPGEN_OPTIONS& options)
{
	if (!texture.IsValid())
	{
		assert(0);
		return;
	}

	TextureDesc desc = texture.GetDesc();

	if (desc.mip_levels < 2)
	{
		assert(0);
		return;
	}


	bool hdr = !IsFormatUnorm(desc.format);

	MipgenPushConstants mipgen;

	if (desc.type == TextureDesc::Type::TEXTURE_1D)
	{
		assert(0); // not implemented
	}
	else if (desc.type == TextureDesc::Type::TEXTURE_2D)
	{

		if (has_flag(desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
		{

			if (desc.array_size > 6)
			{
				// Cubearray
				assert(options.arrayIndex >= 0 && "You should only filter a specific cube in the array for now, so provide its index!");

				switch (filter)
				{
				case MIPGENFILTER_POINT:
					device->EventBegin("GenerateMipChain CubeArray - PointFilter", cmd);
					device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4 : CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
					break;
				case MIPGENFILTER_LINEAR:
					device->EventBegin("GenerateMipChain CubeArray - LinearFilter", cmd);
					device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4 : CSTYPE_GENERATEMIPCHAINCUBEARRAY_UNORM4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
					break;
				default:
					assert(0);
					break;
				}

				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 0),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 1),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 2),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 3),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 4),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}

					mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
					mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
					desc.width = std::max(1u, desc.width / 2);
					desc.height = std::max(1u, desc.height / 2);

					mipgen.outputResolution.x = desc.width;
					mipgen.outputResolution.y = desc.height;
					mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
					mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
					mipgen.arrayIndex = options.arrayIndex;
					mipgen.mipgen_options = 0;
					device->PushConstants(&mipgen, sizeof(mipgen), cmd);

					device->Dispatch(
						std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						6,
						cmd);

					{
						GPUBarrier barriers[] = {
							GPUBarrier::Memory(),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 0),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 1),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 2),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 3),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 4),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}
				}
			}
			else
			{
				// Cubemap
				switch (filter)
				{
				case MIPGENFILTER_POINT:
					device->EventBegin("GenerateMipChain Cube - PointFilter", cmd);
					device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4 : CSTYPE_GENERATEMIPCHAINCUBE_UNORM4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
					break;
				case MIPGENFILTER_LINEAR:
					device->EventBegin("GenerateMipChain Cube - LinearFilter", cmd);
					device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4 : CSTYPE_GENERATEMIPCHAINCUBE_UNORM4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
					break;
				default:
					assert(0); // not implemented
					break;
				}

				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
					mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
					desc.width = std::max(1u, desc.width / 2);
					desc.height = std::max(1u, desc.height / 2);

					mipgen.outputResolution.x = desc.width;
					mipgen.outputResolution.y = desc.height;
					mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
					mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
					mipgen.arrayIndex = 0;
					mipgen.mipgen_options = 0;
					device->PushConstants(&mipgen, sizeof(mipgen), cmd);

					device->Dispatch(
						std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						6,
						cmd);

					GPUBarrier barriers[] = {
						GPUBarrier::Memory(),
					};
					device->Barrier(barriers, arraysize(barriers), cmd);
				}
			}

		}
		else
		{
			// Texture
			switch (filter)
			{
			case MIPGENFILTER_POINT:
				device->EventBegin("GenerateMipChain 2D - PointFilter", cmd);
				device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4 : CSTYPE_GENERATEMIPCHAIN2D_UNORM4], cmd);
				mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
				break;
			case MIPGENFILTER_LINEAR:
				device->EventBegin("GenerateMipChain 2D - LinearFilter", cmd);
				device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAIN2D_FLOAT4 : CSTYPE_GENERATEMIPCHAIN2D_UNORM4], cmd);
				mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
				break;
			case MIPGENFILTER_GAUSSIAN:
			{
				assert(options.gaussian_temp != nullptr); // needed for separate filter!
				device->EventBegin("GenerateMipChain 2D - GaussianFilter", cmd);
				// Gaussian filter is a bit different as we do it in a separable way:
				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					Postprocess_Blur_Gaussian(texture, *options.gaussian_temp, texture, cmd, i, i + 1 , options.wide_gauss);
				}
				device->EventEnd(cmd);
				return;
			}
				break;
			default:
				assert(0);
				break;
			}

			for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
			{
				{
					GPUBarrier barriers[] = {
						GPUBarrier::Image(&texture,texture.desc.layout,ResourceState::UNORDERED_ACCESS,i + 1),
					};
					device->Barrier(barriers, arraysize(barriers), cmd);
				}

				mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
				mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
				desc.width = std::max(1u, desc.width / 2);
				desc.height = std::max(1u, desc.height / 2);

				mipgen.outputResolution.x = desc.width;
				mipgen.outputResolution.y = desc.height;
				mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
				mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
				mipgen.arrayIndex = options.arrayIndex >= 0 ? (uint)options.arrayIndex : 0;
				mipgen.mipgen_options = 0;
				if (options.preserve_coverage)
				{
					mipgen.mipgen_options |= MIPGEN_OPTION_BIT_PRESERVE_COVERAGE;
				}
				device->PushConstants(&mipgen, sizeof(mipgen), cmd);

				device->Dispatch(
					std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
					std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
					1,
					cmd);

				{
					GPUBarrier barriers[] = {
						GPUBarrier::Memory(),
						GPUBarrier::Image(&texture,ResourceState::UNORDERED_ACCESS,texture.desc.layout,i + 1),
					};
					device->Barrier(barriers, arraysize(barriers), cmd);
				}
			}
		}


		device->EventEnd(cmd);
	}
	else if (desc.type == TextureDesc::Type::TEXTURE_3D)
	{
		switch (filter)
		{
		case MIPGENFILTER_POINT:
			device->EventBegin("GenerateMipChain 3D - PointFilter", cmd);
			device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4 : CSTYPE_GENERATEMIPCHAIN3D_UNORM4], cmd);
			mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
			break;
		case MIPGENFILTER_LINEAR:
			device->EventBegin("GenerateMipChain 3D - LinearFilter", cmd);
			device->BindComputeShader(&shaders[hdr ? CSTYPE_GENERATEMIPCHAIN3D_FLOAT4 : CSTYPE_GENERATEMIPCHAIN3D_UNORM4], cmd);
			mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
			break;
		default:
			assert(0); // not implemented
			break;
		}

		for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
		{
			mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
			mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
			desc.width = std::max(1u, desc.width / 2);
			desc.height = std::max(1u, desc.height / 2);
			desc.depth = std::max(1u, desc.depth / 2);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture,texture.desc.layout,ResourceState::UNORDERED_ACCESS,i + 1),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			mipgen.outputResolution.x = desc.width;
			mipgen.outputResolution.y = desc.height;
			mipgen.outputResolution.z = desc.depth;
			mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
			mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
			mipgen.outputResolution_rcp.z = 1.0f / mipgen.outputResolution.z;
			mipgen.arrayIndex = options.arrayIndex >= 0 ? (uint)options.arrayIndex : 0;
			mipgen.mipgen_options = 0;
			device->PushConstants(&mipgen, sizeof(mipgen), cmd);

			device->Dispatch(
				std::max(1u, (desc.width + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				std::max(1u, (desc.height + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				std::max(1u, (desc.depth + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&texture,ResourceState::UNORDERED_ACCESS,texture.desc.layout,i + 1),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}


		device->EventEnd(cmd);
	}
	else
	{
		assert(0);
	}
}

void CopyTexture2D(const Texture& dst, int DstMIP, int DstX, int DstY, const Texture& src, int SrcMIP, CommandList cmd, BORDEREXPANDSTYLE borderExpand)
{
	const TextureDesc& desc_dst = dst.GetDesc();
	const TextureDesc& desc_src = src.GetDesc();

	assert(has_flag(desc_dst.bind_flags, BindFlag::UNORDERED_ACCESS));
	assert(has_flag(desc_src.bind_flags, BindFlag::SHADER_RESOURCE));

	bool hdr = !IsFormatUnorm(desc_dst.format);

	if (borderExpand == BORDEREXPAND_DISABLE)
	{
		if (hdr)
		{
			device->EventBegin("CopyTexture_FLOAT4", cmd);
			device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_FLOAT4], cmd);
		}
		else
		{
			device->EventBegin("CopyTexture_UNORM4", cmd);
			device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_UNORM4], cmd);
		}
	}
	else
	{
		if (hdr)
		{
			device->EventBegin("CopyTexture_BORDEREXPAND_FLOAT4", cmd);
			device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_FLOAT4_BORDEREXPAND], cmd);
		}
		else
		{
			device->EventBegin("CopyTexture_BORDEREXPAND_UNORM4", cmd);
			device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_UNORM4_BORDEREXPAND], cmd);
		}
	}

	CopyTextureCB cb;
	cb.xCopyDest.x = DstX;
	cb.xCopyDest.y = DstY;
	cb.xCopySrcSize.x = desc_src.width >> SrcMIP;
	cb.xCopySrcSize.y = desc_src.height >> SrcMIP;
	cb.xCopySrcMIP = SrcMIP;
	cb.xCopyBorderExpandStyle = (uint)borderExpand;
	device->PushConstants(&cb, sizeof(cb), cmd);

	device->BindResource(&src, 0, cmd);

	device->BindUAV(&dst, 0, cmd, DstMIP);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&dst,dst.desc.layout,ResourceState::UNORDERED_ACCESS, DstMIP),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch((cb.xCopySrcSize.x + 7) / 8, (cb.xCopySrcSize.y + 7) / 8, 1, cmd);


	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&dst,ResourceState::UNORDERED_ACCESS,dst.desc.layout, DstMIP),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}


void RayTraceScene(
	const Scene& scene,
	const Texture& output,
	int accumulation_sample, 
	CommandList cmd,
	uint8_t instanceInclusionMask,
	const Texture* output_albedo,
	const Texture* output_normal
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	device->EventBegin("RayTraceScene", cmd);
	auto range = wi::profiler::BeginRangeGPU("RayTraceScene", cmd);

	const TextureDesc& desc = output.GetDesc();

	BindCommonResources(cmd);

	const XMFLOAT4& halton = wi::math::GetHaltonSequence(accumulation_sample);
	RaytracingCB cb;
	cb.xTracePixelOffset = XMFLOAT2(halton.x, halton.y);
	cb.xTraceAccumulationFactor = 1.0f / ((float)accumulation_sample + 1.0f);
	cb.xTraceResolution.x = desc.width;
	cb.xTraceResolution.y = desc.height;
	cb.xTraceResolution_rcp.x = 1.0f / cb.xTraceResolution.x;
	cb.xTraceResolution_rcp.y = 1.0f / cb.xTraceResolution.y;
	cb.xTraceUserData.x = raytraceBounceCount;
	cb.xTraceUserData.y = instanceInclusionMask;
	cb.xTraceSampleIndex = (uint32_t)accumulation_sample;
	device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(RaytracingCB), cmd);

	device->BindComputeShader(&shaders[CSTYPE_RAYTRACE], cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	if (output_albedo != nullptr)
	{
		device->BindUAV(output_albedo, 1, cmd);
	}
	if (output_normal != nullptr)
	{
		device->BindUAV(output_normal, 2, cmd);
	}

	device->Dispatch(
		(desc.width + RAYTRACING_LAUNCH_BLOCKSIZE - 1) / RAYTRACING_LAUNCH_BLOCKSIZE,
		(desc.height + RAYTRACING_LAUNCH_BLOCKSIZE - 1) / RAYTRACING_LAUNCH_BLOCKSIZE,
		1,
		cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	wi::profiler::EndRange(range);
	device->EventEnd(cmd); // RayTraceScene
}
void RayTraceSceneBVH(const Scene& scene, CommandList cmd)
{
	device->EventBegin("RayTraceSceneBVH", cmd);
	device->BindPipelineState(&PSO_debug[DEBUGRENDERING_RAYTRACE_BVH], cmd);
	device->Draw(3, 0, cmd);
	device->EventEnd(cmd);
}

void RefreshLightmaps(const Scene& scene, CommandList cmd, uint8_t instanceInclusionMask)
{
	if (scene.lightmap_refresh_needed.load())
	{
		auto range = wi::profiler::BeginRangeGPU("Lightmap Processing", cmd);

		if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
			return;

		BindCommonResources(cmd);

		// Render lightmaps for each object:
		for (uint32_t i = 0; i < scene.objects.GetCount(); ++i)
		{
			const ObjectComponent& object = scene.objects[i];
			if (!object.lightmap.IsValid())
				continue;

			if (object.IsLightmapRenderRequested())
			{
				device->EventBegin("RenderObjectLightMap", cmd);

				const MeshComponent& mesh = scene.meshes[object.mesh_index];
				assert(!mesh.vertex_atlas.empty());
				assert(mesh.vb_atl.IsValid());

				const TextureDesc& desc = object.lightmap.GetDesc();

				if (object.lightmapIterationCount == 0)
				{
					device->RenderPassBegin(&object.renderpass_lightmap_clear, cmd);
				}
				else
				{
					device->RenderPassBegin(&object.renderpass_lightmap_accumulate, cmd);
				}

				Viewport vp;
				vp.width = (float)desc.width;
				vp.height = (float)desc.height;
				device->BindViewports(1, &vp, cmd);

				MiscCB misccb;
				misccb.g_xTransform = object.worldMatrix;

				device->BindDynamicConstantBuffer(misccb, CB_GETBINDSLOT(MiscCB), cmd);

				const GPUBuffer* vbs[] = {
					&mesh.generalBuffer,
					&mesh.generalBuffer,
				};
				uint32_t strides[] = {
					sizeof(MeshComponent::Vertex_POS),
					sizeof(MeshComponent::Vertex_TEX),
				};
				uint64_t offsets[] = {
					mesh.vb_pos_nor_wind.offset,
					mesh.vb_atl.offset,
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);
				device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

				RaytracingCB cb;
				cb.xTraceResolution.x = desc.width;
				cb.xTraceResolution.y = desc.height;
				cb.xTraceResolution_rcp.x = 1.0f / cb.xTraceResolution.x;
				cb.xTraceResolution_rcp.y = 1.0f / cb.xTraceResolution.y;
				XMFLOAT4 halton = wi::math::GetHaltonSequence(object.lightmapIterationCount); // for jittering the rasterization (good for eliminating atlas border artifacts)
				cb.xTracePixelOffset.x = (halton.x * 2 - 1) * cb.xTraceResolution_rcp.x;
				cb.xTracePixelOffset.y = (halton.y * 2 - 1) * cb.xTraceResolution_rcp.y;
				cb.xTracePixelOffset.x *= 1.4f;	// boost the jitter by a bit
				cb.xTracePixelOffset.y *= 1.4f;	// boost the jitter by a bit
				cb.xTraceAccumulationFactor = 1.0f / (object.lightmapIterationCount + 1.0f); // accumulation factor (alpha)
				cb.xTraceUserData.x = raytraceBounceCount;
				cb.xTraceUserData.y = instanceInclusionMask;
				cb.xTraceSampleIndex = object.lightmapIterationCount;
				device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(RaytracingCB), cmd);

				device->BindPipelineState(&PSO_renderlightmap, cmd);

				device->DrawIndexedInstanced((uint32_t)mesh.indices.size(), 1, 0, 0, 0, cmd);
				object.lightmapIterationCount++;

				device->RenderPassEnd(cmd);

				device->EventEnd(cmd);
			}
		}

		scene.lightmap_refresh_needed.store(false);
		wi::profiler::EndRange(range);
	}
}

void BindCommonResources(CommandList cmd)
{
	device->BindConstantBuffer(&constantBuffers[CBTYPE_FRAME], CBSLOT_RENDERER_FRAME, cmd);
}

void BindCameraCB(
	const CameraComponent& camera,
	const CameraComponent& camera_previous,
	const CameraComponent& camera_reflection,
	CommandList cmd
)
{
	CameraCB cb;

	XMStoreFloat4x4(&cb.view_projection, camera.GetViewProjection());
	XMStoreFloat4x4(&cb.view, camera.GetView());
	XMStoreFloat4x4(&cb.projection, camera.GetProjection());
	cb.position = camera.Eye;
	cb.distance_from_origin = XMVectorGetX(XMVector3Length(XMLoadFloat3(&cb.position)));
	XMStoreFloat4x4(&cb.inverse_view, camera.GetInvView());
	XMStoreFloat4x4(&cb.inverse_projection, camera.GetInvProjection());
	XMStoreFloat4x4(&cb.inverse_view_projection, camera.GetInvViewProjection());
	cb.forward = camera.At;
	cb.up = camera.Up;
	cb.z_near = camera.zNearP;
	cb.z_far = camera.zFarP;
	cb.z_near_rcp = 1.0f / std::max(0.0001f, cb.z_near);
	cb.z_far_rcp = 1.0f / std::max(0.0001f, cb.z_far);
	cb.z_range = abs(cb.z_far - cb.z_near);
	cb.z_range_rcp = 1.0f / std::max(0.0001f, cb.z_range);
	cb.clip_plane = camera.clipPlane;

	static_assert(arraysize(camera.frustum.planes) == arraysize(cb.frustum.planes), "Mismatch!");
	for (int i = 0; i < arraysize(camera.frustum.planes); ++i)
	{
		cb.frustum.planes[i] = camera.frustum.planes[i];
	}

	cb.temporalaa_jitter = camera.jitter;
	cb.temporalaa_jitter_prev = camera_previous.jitter;

	XMStoreFloat4x4(&cb.previous_view, camera_previous.GetView());
	XMStoreFloat4x4(&cb.previous_projection, camera_previous.GetProjection());
	XMStoreFloat4x4(&cb.previous_view_projection, camera_previous.GetViewProjection());
	XMStoreFloat4x4(&cb.previous_inverse_view_projection, camera_previous.GetInvViewProjection());
	XMStoreFloat4x4(&cb.reflection_view_projection, camera_reflection.GetViewProjection());
	XMStoreFloat4x4(&cb.reprojection, camera.GetInvViewProjection() * camera_previous.GetViewProjection());

	cb.focal_length = camera.focal_length;
	cb.aperture_size = camera.aperture_size;
	cb.aperture_shape = camera.aperture_shape;

	cb.canvas_size = float2(camera.canvas.GetLogicalWidth(), camera.canvas.GetLogicalHeight());
	cb.canvas_size_rcp = float2(1.0f / cb.canvas_size.x, 1.0f / cb.canvas_size.y);
	cb.internal_resolution = uint2((uint)camera.width, (uint)camera.height);
	cb.internal_resolution_rcp = float2(1.0f / cb.internal_resolution.x, 1.0f / cb.internal_resolution.y);

	cb.entity_culling_tilecount = GetEntityCullingTileCount(cb.internal_resolution);
	cb.sample_count = camera.sample_count;
	cb.visibility_tilecount = GetVisibilityTileCount(cb.internal_resolution);
	cb.visibility_tilecount_flat = cb.visibility_tilecount.x * cb.visibility_tilecount.y;

	cb.texture_primitiveID_index = camera.texture_primitiveID_index;
	cb.texture_depth_index = camera.texture_depth_index;
	cb.texture_lineardepth_index = camera.texture_lineardepth_index;
	cb.texture_velocity_index = camera.texture_velocity_index;
	cb.texture_normal_index = camera.texture_normal_index;
	cb.texture_roughness_index = camera.texture_roughness_index;
	cb.buffer_entitytiles_opaque_index = camera.buffer_entitytiles_opaque_index;
	cb.buffer_entitytiles_transparent_index = camera.buffer_entitytiles_transparent_index;
	cb.texture_reflection_index = camera.texture_reflection_index;
	cb.texture_refraction_index = camera.texture_refraction_index;
	cb.texture_waterriples_index = camera.texture_waterriples_index;
	cb.texture_ao_index = camera.texture_ao_index;
	cb.texture_ssr_index = camera.texture_ssr_index;
	cb.texture_rtshadow_index = camera.texture_rtshadow_index;
	cb.texture_surfelgi_index = camera.texture_surfelgi_index;
	cb.texture_depth_index_prev = camera_previous.texture_depth_index;

	device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);
}

void CreateLuminanceResources(LuminanceResources& res, XMUINT2 resolution)
{
	float values[LUMINANCE_NUM_HISTOGRAM_BINS + 1 + 1] = {}; // 1 exposure + 1 luminance value + histogram
	GPUBufferDesc desc;
	desc.size = sizeof(values);
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	device->CreateBuffer(&desc, values, &res.luminance);
	device->SetName(&res.luminance, "luminance");
}
void ComputeLuminance(
	const LuminanceResources& res,
	const Texture& sourceImage,
	CommandList cmd,
	float adaption_rate,
	float eyeadaptionkey
)
{
	device->EventBegin("Compute Luminance", cmd);
	auto range = wi::profiler::BeginRangeGPU("Luminance", cmd);

	PostProcess postprocess;
	postprocess.resolution.x = sourceImage.desc.width / 2;
	postprocess.resolution.y = sourceImage.desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	luminance_adaptionrate = adaption_rate;
	luminance_log_min = -10.0f;
	luminance_log_max = 2.0f;
	luminance_log_range = luminance_log_max - luminance_log_min;
	luminance_log_range_rcp = 1.0f / luminance_log_range;
	luminance_pixelcount = float(postprocess.resolution.x * postprocess.resolution.y);
	luminance_eyeadaptionkey = eyeadaptionkey;

	device->BindUAV(&res.luminance, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&res.luminance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Pass 1 : Compute log luminance and reduction
	{
		device->BindComputeShader(&shaders[CSTYPE_LUMINANCE_PASS1], cmd);
		device->BindResource(&sourceImage, 0, cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(
			(postprocess.resolution.x + LUMINANCE_BLOCKSIZE - 1) / LUMINANCE_BLOCKSIZE,
			(postprocess.resolution.y + LUMINANCE_BLOCKSIZE - 1) / LUMINANCE_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}

	// Pass 2 : Reduce into 1x1 texture
	{
		device->BindComputeShader(&shaders[CSTYPE_LUMINANCE_PASS2], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(1, 1, 1, cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.luminance, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateBloomResources(BloomResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.format = Format::R11G11B10_FLOAT;
	desc.width = resolution.x / 4;
	desc.height = resolution.y / 4;
	desc.mip_levels = std::min(5u, (uint32_t)std::log2(std::max(desc.width, desc.height)));
	device->CreateTexture(&desc, nullptr, &res.texture_bloom);
	device->SetName(&res.texture_bloom, "bloom.texture_bloom");
	device->CreateTexture(&desc, nullptr, &res.texture_temp);
	device->SetName(&res.texture_temp, "bloom.texture_temp");

	for (uint32_t i = 0; i < res.texture_bloom.desc.mip_levels; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&res.texture_bloom, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_temp, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_bloom, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_temp, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}
}
void ComputeBloom(
	const BloomResources& res,
	const Texture& input,
	CommandList cmd,
	float threshold,
	float exposure,
	const GPUBuffer* buffer_luminance
)
{
	device->EventBegin("Bloom", cmd);
	auto range = wi::profiler::BeginRangeGPU("Bloom", cmd);

	// Separate bright parts of image to bloom texture:
	{
		device->EventBegin("Bloom Separate", cmd);

		const TextureDesc& desc = res.texture_bloom.GetDesc();

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_BLOOMSEPARATE], cmd);

		Bloom bloom;
		bloom.resolution_rcp.x = 1.0f / desc.width;
		bloom.resolution_rcp.y = 1.0f / desc.height;
		bloom.threshold = threshold;
		bloom.exposure = exposure;
		bloom.texture_input = device->GetDescriptorIndex(&input, SubresourceType::SRV);
		bloom.texture_output = device->GetDescriptorIndex(&res.texture_bloom, SubresourceType::UAV);
		bloom.buffer_input_luminance = device->GetDescriptorIndex((buffer_luminance == nullptr) ? &luminance_dummy : buffer_luminance, SubresourceType::SRV);
		device->PushConstants(&bloom, sizeof(bloom), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_bloom, res.texture_bloom.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_bloom, ResourceState::UNORDERED_ACCESS, res.texture_bloom.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventBegin("Bloom Mipchain", cmd);
	MIPGEN_OPTIONS mipopt;
	mipopt.gaussian_temp = &res.texture_temp;
	mipopt.wide_gauss = true;
	GenerateMipChain(res.texture_bloom, MIPGENFILTER_GAUSSIAN, cmd, mipopt);
	device->EventEnd(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void ComputeShadingRateClassification(
	const Texture& output,
	const Texture& debugUAV,
	CommandList cmd
)
{
	device->EventBegin("ComputeShadingRateClassification", cmd);
	auto range = wi::profiler::BeginRangeGPU("ComputeShadingRateClassification", cmd);

	if (GetVariableRateShadingClassificationDebug())
	{
		device->BindUAV(&debugUAV, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&debugUAV, debugUAV.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindComputeShader(&shaders[CSTYPE_SHADINGRATECLASSIFICATION_DEBUG], cmd);
	}
	else
	{
		device->BindComputeShader(&shaders[CSTYPE_SHADINGRATECLASSIFICATION], cmd);
	}

	const TextureDesc& desc = output.GetDesc();

	ShadingRateClassification shadingrate = {}; // zero init the shading rates!
	shadingrate.TileSize = device->GetVariableRateShadingTileSize();
	device->WriteShadingRateValue(ShadingRate::RATE_1X1, &shadingrate.SHADING_RATE_1X1);
	device->WriteShadingRateValue(ShadingRate::RATE_1X2, &shadingrate.SHADING_RATE_1X2);
	device->WriteShadingRateValue(ShadingRate::RATE_2X1, &shadingrate.SHADING_RATE_2X1);
	device->WriteShadingRateValue(ShadingRate::RATE_2X2, &shadingrate.SHADING_RATE_2X2);
	device->WriteShadingRateValue(ShadingRate::RATE_2X4, &shadingrate.SHADING_RATE_2X4);
	device->WriteShadingRateValue(ShadingRate::RATE_4X2, &shadingrate.SHADING_RATE_4X2);
	device->WriteShadingRateValue(ShadingRate::RATE_4X4, &shadingrate.SHADING_RATE_4X4);
	device->PushConstants(&shadingrate, sizeof(shadingrate), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	// Whole threadgroup for each tile:
	device->Dispatch(desc.width, desc.height, 1, cmd);

	if (GetVariableRateShadingClassificationDebug())
	{
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&debugUAV, ResourceState::UNORDERED_ACCESS, debugUAV.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}


	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateVisibilityResources(VisibilityResources& res, XMUINT2 resolution)
{
	res.tile_count = GetVisibilityTileCount(resolution);
	{
		GPUBufferDesc desc;
		desc.stride = sizeof(ShaderTypeBin);
		desc.size = desc.stride * (MaterialComponent::SHADERTYPE_COUNT + 1); // +1 for sky
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS | BindFlag::CONSTANT_BUFFER;
		desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED | ResourceMiscFlag::INDIRECT_ARGS;
		bool success = device->CreateBuffer(&desc, nullptr, &res.bins);
		assert(success);
		device->SetName(&res.bins, "res.bins");

		desc.stride = sizeof(VisibilityTile);
		desc.size = desc.stride * res.tile_count.x * res.tile_count.y * (MaterialComponent::SHADERTYPE_COUNT + 1); // +1 for sky
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
		success = device->CreateBuffer(&desc, nullptr, &res.binned_tiles);
		assert(success);
		device->SetName(&res.binned_tiles, "res.binned_tiles");
	}
	{
		TextureDesc desc;
		desc.width = resolution.x;
		desc.height = resolution.y;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

		desc.format = Format::R16G16_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.texture_normals);
		device->SetName(&res.texture_normals, "res.texture_normals");

		desc.format = Format::R8_UNORM;
		device->CreateTexture(&desc, nullptr, &res.texture_roughness);
		device->SetName(&res.texture_roughness, "res.texture_roughness");

		desc.format = Format::R32G32B32A32_UINT;
		device->CreateTexture(&desc, nullptr, &res.texture_payload_0);
		device->SetName(&res.texture_payload_0, "res.texture_payload_0");
		device->CreateTexture(&desc, nullptr, &res.texture_payload_1);
		device->SetName(&res.texture_payload_1, "res.texture_payload_1");
	}
}
void Visibility_Prepare(
	const VisibilityResources& res,
	const Texture& input_primitiveID, // can be MSAA
	CommandList cmd
)
{
	device->EventBegin("Visibility_Prepare", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Prepare", cmd);

	BindCommonResources(cmd);

	// Beginning barriers, clears:
	{
		barrier_stack.push_back(GPUBarrier::Buffer(&res.bins, ResourceState::CONSTANT_BUFFER | ResourceState::INDIRECT_ARGUMENT, ResourceState::UNORDERED_ACCESS));
		barrier_stack.push_back(GPUBarrier::Buffer(&res.binned_tiles, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS));
		barrier_stack_flush(cmd);
		device->ClearUAV(&res.bins, 0, cmd);
		barrier_stack.push_back(GPUBarrier::Memory(&res.bins));
		barrier_stack_flush(cmd);
	}

	// Resolve:
	//	PrimitiveID -> depth, lineardepth
	//	Binning classification
	{
		device->EventBegin("Resolve", cmd);
		const bool msaa = input_primitiveID.GetDesc().sample_count > 1;

		device->BindResource(&input_primitiveID, 0, cmd);

		device->BindUAV(&res.bins, 0, cmd);
		device->BindUAV(&res.binned_tiles, 1, cmd);

		GPUResource unbind;
		if (res.depthbuffer)
		{
			device->BindUAV(res.depthbuffer, 3, cmd, 0);
			device->BindUAV(res.depthbuffer, 4, cmd, 1);
			device->BindUAV(res.depthbuffer, 5, cmd, 2);
			device->BindUAV(res.depthbuffer, 6, cmd, 3);
			device->BindUAV(res.depthbuffer, 7, cmd, 4);
			barrier_stack.push_back(GPUBarrier::Image(res.depthbuffer, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 3, cmd);
			device->BindUAV(&unbind, 4, cmd);
			device->BindUAV(&unbind, 5, cmd);
			device->BindUAV(&unbind, 6, cmd);
			device->BindUAV(&unbind, 7, cmd);
		}
		if (res.lineardepth)
		{
			device->BindUAV(res.lineardepth, 8, cmd, 0);
			device->BindUAV(res.lineardepth, 9, cmd, 1);
			device->BindUAV(res.lineardepth, 10, cmd, 2);
			device->BindUAV(res.lineardepth, 11, cmd, 3);
			device->BindUAV(res.lineardepth, 12, cmd, 4);
			barrier_stack.push_back(GPUBarrier::Image(res.lineardepth, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 8, cmd);
			device->BindUAV(&unbind, 9, cmd);
			device->BindUAV(&unbind, 10, cmd);
			device->BindUAV(&unbind, 11, cmd);
			device->BindUAV(&unbind, 12, cmd);
		}
		if (res.primitiveID_resolved)
		{
			device->BindUAV(res.primitiveID_resolved, 13, cmd);
			barrier_stack.push_back(GPUBarrier::Image(res.primitiveID_resolved, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 13, cmd);
		}
		barrier_stack_flush(cmd);

		device->BindComputeShader(&shaders[msaa ? CSTYPE_VISIBILITY_RESOLVE_MSAA : CSTYPE_VISIBILITY_RESOLVE], cmd);

		device->Dispatch(
			res.tile_count.x,
			res.tile_count.y,
			1,
			cmd
		);

		if (res.depthbuffer)
		{
			barrier_stack.push_back(GPUBarrier::Image(res.depthbuffer, ResourceState::UNORDERED_ACCESS, res.depthbuffer->desc.layout));
		}
		if (res.lineardepth)
		{
			barrier_stack.push_back(GPUBarrier::Image(res.lineardepth, ResourceState::UNORDERED_ACCESS, res.lineardepth->desc.layout));
		}
		if (res.primitiveID_resolved)
		{
			barrier_stack.push_back(GPUBarrier::Image(res.primitiveID_resolved, ResourceState::UNORDERED_ACCESS, res.primitiveID_resolved->desc.layout));
		}
		barrier_stack.push_back(GPUBarrier::Memory(&res.bins));
		barrier_stack_flush(cmd);

		device->EventEnd(cmd);
	}

	// Indirect prepare:
	{
		device->EventBegin("Indirect Prepare", cmd);
		device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_INDIRECT_PREPARE], cmd);
		device->BindUAV(&res.bins, 0, cmd);
		device->Dispatch(1, 1, 1, cmd);
		device->EventEnd(cmd);
	}

	barrier_stack.push_back(GPUBarrier::Buffer(&res.bins, ResourceState::UNORDERED_ACCESS, ResourceState::CONSTANT_BUFFER | ResourceState::INDIRECT_ARGUMENT));
	barrier_stack.push_back(GPUBarrier::Buffer(&res.binned_tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
	barrier_stack_flush(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Surface(
	const VisibilityResources& res,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Surface", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Surface", cmd);

	BindCommonResources(cmd);

	barrier_stack.push_back(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_normals, res.texture_normals.desc.layout, ResourceState::UNORDERED_ACCESS));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_roughness, res.texture_roughness.desc.layout, ResourceState::UNORDERED_ACCESS));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_payload_0, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_payload_1, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS));
	barrier_stack_flush(cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindUAV(&output, 0, cmd);
	device->BindUAV(&res.texture_normals, 1, cmd);
	device->BindUAV(&res.texture_roughness, 2, cmd);
	device->BindUAV(&res.texture_payload_0, 3, cmd);
	device->BindUAV(&res.texture_payload_1, 4, cmd);

	// surface dispatches per material type:
	device->EventBegin("Surface parameters", cmd);
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SURFACE_PERMUTATION_BEGIN + i], cmd);
		device->BindConstantBuffer(&res.bins, 10, cmd, i * sizeof(ShaderTypeBin));
		device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
	}
	device->EventEnd(cmd);

	// sky dispatch:
	device->EventBegin("Sky", cmd);
	device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SKY], cmd);
	device->BindConstantBuffer(&res.bins, 10, cmd, MaterialComponent::SHADERTYPE_COUNT * sizeof(ShaderTypeBin));
	device->DispatchIndirect(&res.bins, MaterialComponent::SHADERTYPE_COUNT * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
	device->EventEnd(cmd);

	// Ending barriers:
	//	These resources will be used by other post processing effects
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_normals, ResourceState::UNORDERED_ACCESS, res.texture_normals.desc.layout));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_roughness, ResourceState::UNORDERED_ACCESS, res.texture_roughness.desc.layout));
	barrier_stack_flush(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Surface_Reduced(
	const VisibilityResources& res,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Surface_Reduced", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Surface_Reduced", cmd);

	BindCommonResources(cmd);

	barrier_stack.push_back(GPUBarrier::Image(&res.texture_normals, res.texture_normals.desc.layout, ResourceState::UNORDERED_ACCESS));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_roughness, res.texture_roughness.desc.layout, ResourceState::UNORDERED_ACCESS));
	barrier_stack_flush(cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindUAV(&res.texture_normals, 1, cmd);
	device->BindUAV(&res.texture_roughness, 2, cmd);

	// surface dispatches per material type:
	device->EventBegin("Surface parameters", cmd);
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		if (i == MaterialComponent::SHADERTYPE_UNLIT)
			continue; // this won't need surface parameter write out
		device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SURFACE_REDUCED_PERMUTATION_BEGIN + i], cmd);
		device->BindConstantBuffer(&res.bins, 10, cmd, i * sizeof(ShaderTypeBin));
		device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
	}
	device->EventEnd(cmd);

	// Ending barriers:
	//	These resources will be used by other post processing effects
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_normals, ResourceState::UNORDERED_ACCESS, res.texture_normals.desc.layout));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_roughness, ResourceState::UNORDERED_ACCESS, res.texture_roughness.desc.layout));
	barrier_stack_flush(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Shade(
	const VisibilityResources& res,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Shade", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Shade", cmd);

	BindCommonResources(cmd);

	barrier_stack.push_back(GPUBarrier::Image(&res.texture_payload_0, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
	barrier_stack.push_back(GPUBarrier::Image(&res.texture_payload_1, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
	barrier_stack_flush(cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindResource(&res.texture_payload_0, 2, cmd);
	device->BindResource(&res.texture_payload_1, 3, cmd);
	device->BindUAV(&output, 0, cmd);

	// shading dispatches per material type:
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		if (i == MaterialComponent::SHADERTYPE_UNLIT)
			continue; // the unlit shader is special, it had already written out its final color in the surface shader
		device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SHADE_PERMUTATION_BEGIN + i], cmd);
		device->BindConstantBuffer(&res.bins, 10, cmd, i * sizeof(ShaderTypeBin));
		device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Velocity(
	const VisibilityResources& res,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Velocity", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Velocity", cmd);

	BindCommonResources(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_VELOCITY], cmd);
	device->BindUAV(&output, 0, cmd);
	device->Dispatch(
		(output.desc.width + 7u) / 8u,
		(output.desc.height + 7u) / 8u,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateSurfelGIResources(SurfelGIResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.format = Format::R11G11B10_FLOAT;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.result);
	device->SetName(&res.result, "surfelGI.result");
}
void SurfelGI_Coverage(
	const SurfelGIResources& res,
	const Scene& scene,
	const Texture& debugUAV,
	CommandList cmd
)
{
	device->EventBegin("SurfelGI - Coverage", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("SurfelGI - Coverage", cmd);


	// Coverage:
	{
		device->EventBegin("Coverage", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_COVERAGE], cmd);

		SurfelDebugPushConstants push;
		push.debug = GetSurfelGIDebugEnabled();
		device->PushConstants(&push, sizeof(push), cmd);

		device->BindResource(&scene.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelGridBuffer, 1, cmd);
		device->BindResource(&scene.surfelCellBuffer, 2, cmd);
		device->BindResource(&scene.surfelMomentsTexture[1], 3, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelDataBuffer,
			&scene.surfelDeadBuffer,
			&scene.surfelAliveBuffer[1],
			&scene.surfelStatsBuffer,
			&res.result,
			&debugUAV
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelStatsBuffer, ResourceState::INDIRECT_ARGUMENT, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.result, res.result.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&debugUAV, debugUAV.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

#ifdef SURFEL_COVERAGE_HALFRES
		device->Dispatch(
			(res.result.desc.width / 2 + 15) / 16,
			(res.result.desc.height / 2 + 15) / 16,
			1,
			cmd
		);
#else
		device->Dispatch(
			(res.result.desc.width + 15) / 16,
			(res.result.desc.height + 15) / 16,
			1,
			cmd
		);
#endif // SURFEL_COVERAGE_HALFRES

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.result, ResourceState::UNORDERED_ACCESS, res.result.desc.layout),
				GPUBarrier::Image(&debugUAV, ResourceState::UNORDERED_ACCESS, debugUAV.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// surfel count -> indirect args (for next frame):
	{
		device->EventBegin("Indirect args", cmd);
		const GPUResource* uavs[] = {
			&scene.surfelStatsBuffer,
			&scene.surfelIndirectBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_INDIRECTPREPARE], cmd);
		device->Dispatch(1, 1, 1, cmd);

		device->EventEnd(cmd);
	}


	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void SurfelGI(
	const SurfelGIResources& res,
	const Scene& scene,
	CommandList cmd,
	uint8_t instanceInclusionMask
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	auto prof_range = wi::profiler::BeginRangeGPU("SurfelGI", cmd);
	device->EventBegin("SurfelGI", cmd);

	BindCommonResources(cmd);


	// Grid reset:
	{
		device->EventBegin("Grid Reset", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_GRIDRESET], cmd);

		const GPUResource* uavs[] = {
			&scene.surfelGridBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelGridBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(SURFEL_TABLE_SIZE + 63) / 64,
			1,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	// Update:
	{
		device->EventBegin("Update", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_UPDATE], cmd);

		device->BindResource(&scene.surfelDataBuffer, 0, cmd);
		device->BindResource(&scene.surfelAliveBuffer[0], 1, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelBuffer,
			&scene.surfelGridBuffer,
			&scene.surfelAliveBuffer[1],
			&scene.surfelDeadBuffer,
			&scene.surfelStatsBuffer,
			&scene.surfelRayBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->DispatchIndirect(&scene.surfelIndirectBuffer, SURFEL_INDIRECT_OFFSET_ITERATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Buffer(&scene.surfelBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Grid offsets:
	{
		device->EventBegin("Grid Offsets", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_GRIDOFFSETS], cmd);

		const GPUResource* uavs[] = {
			&scene.surfelGridBuffer,
			&scene.surfelCellBuffer,
			&scene.surfelStatsBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelCellBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Buffer(&scene.surfelStatsBuffer, ResourceState::INDIRECT_ARGUMENT, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(SURFEL_TABLE_SIZE + 63) / 64,
			1,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Buffer(&scene.surfelStatsBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
		}

	// Binning:
	{
		device->EventBegin("Binning", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_BINNING], cmd);

		device->BindResource(&scene.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelAliveBuffer[0], 1, cmd);
		device->BindResource(&scene.surfelStatsBuffer, 2, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelGridBuffer,
			&scene.surfelCellBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.surfelIndirectBuffer, SURFEL_INDIRECT_OFFSET_ITERATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Buffer(&scene.surfelGridBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Buffer(&scene.surfelCellBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}



	// Raytracing:
	{
		device->EventBegin("Raytrace", cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_RAYTRACE], cmd);

		PushConstantsSurfelRaytrace push;
		push.instanceInclusionMask = instanceInclusionMask;
		device->PushConstants(&push, sizeof(push), cmd);

		device->BindResource(&scene.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelStatsBuffer, 1, cmd);
		device->BindResource(&scene.surfelGridBuffer, 2, cmd);
		device->BindResource(&scene.surfelCellBuffer, 3, cmd);
		device->BindResource(&scene.surfelAliveBuffer[0], 4, cmd);
		device->BindResource(&scene.surfelMomentsTexture[0], 5, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelRayBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.surfelIndirectBuffer, SURFEL_INDIRECT_OFFSET_RAYTRACE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Buffer(&scene.surfelRayBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}



	// Integrate rays:
	{
		device->EventBegin("Integrate", cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_INTEGRATE], cmd);

		device->BindResource(&scene.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelStatsBuffer, 1, cmd);
		device->BindResource(&scene.surfelGridBuffer, 2, cmd);
		device->BindResource(&scene.surfelCellBuffer, 3, cmd);
		device->BindResource(&scene.surfelAliveBuffer[0], 4, cmd);
		device->BindResource(&scene.surfelMomentsTexture[0], 5, cmd);
		device->BindResource(&scene.surfelRayBuffer, 6, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelDataBuffer,
			&scene.surfelMomentsTexture[1],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelDataBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&scene.surfelMomentsTexture[1], ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->DispatchIndirect(&scene.surfelIndirectBuffer, SURFEL_INDIRECT_OFFSET_INTEGRATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&scene.surfelMomentsTexture[1], ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}

void DDGI(
	const wi::scene::Scene& scene,
	wi::graphics::CommandList cmd,
	uint8_t instanceInclusionMask
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	auto prof_range = wi::profiler::BeginRangeGPU("DDGI", cmd);
	device->EventBegin("DDGI", cmd);

	BindCommonResources(cmd);

	DDGIPushConstants push;
	push.instanceInclusionMask = instanceInclusionMask;
	push.frameIndex = scene.ddgi_frameIndex;
	push.rayCount = std::min(GetDDGIRayCount(), DDGI_MAX_RAYCOUNT);

	// Raytracing:
	{
		device->EventBegin("Raytrace", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_RAYTRACE], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		MiscCB cb = {};
		float angle = wi::random::GetRandom(0.0f, 1.0f) * XM_2PI;
		XMVECTOR axis = XMVectorSet(
			wi::random::GetRandom(-1.0f, 1.0f),
			wi::random::GetRandom(-1.0f, 1.0f),
			wi::random::GetRandom(-1.0f, 1.0f),
			0
		);
		axis = XMVector3Normalize(axis);
		XMStoreFloat4x4(&cb.g_xTransform, XMMatrixRotationAxis(axis, angle));
		device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(MiscCB), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgiRayBuffer
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(DDGI_PROBE_COUNT, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&scene.ddgiRayBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Image(&scene.ddgiColorTexture[1], ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&scene.ddgiDepthTexture[1], ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&scene.ddgiOffsetBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Update:
	{
		device->EventBegin("Update", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_UPDATE], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		const GPUResource* res[] = {
			&scene.ddgiRayBuffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgiColorTexture[1],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(DDGI_PROBE_COUNT, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	// Update Depth:
	{
		device->EventBegin("Update Depth", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_UPDATE_DEPTH], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		const GPUResource* res[] = {
			&scene.ddgiRayBuffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgiDepthTexture[1],
			&scene.ddgiOffsetBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(DDGI_PROBE_COUNT, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&scene.ddgiColorTexture[1], ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Image(&scene.ddgiDepthTexture[1], ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Buffer(&scene.ddgiOffsetBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}

void Postprocess_Blur_Gaussian(
	const Texture& input,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	int mip_src,
	int mip_dst,
	bool wide
)
{
	device->EventBegin("Postprocess_Blur_Gaussian", cmd);

	SHADERTYPE cs = CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4;
	switch (output.GetDesc().format)
	{
	case Format::R16_UNORM:
	case Format::R8_UNORM:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_UNORM1 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_UNORM1;
		break;
	case Format::R16_FLOAT:
	case Format::R32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT1 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT1;
		break;
	case Format::R16G16B16A16_UNORM:
	case Format::R8G8B8A8_UNORM:
	case Format::B8G8R8A8_UNORM:
	case Format::R10G10B10A2_UNORM:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_UNORM4 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_UNORM4;
		break;
	case Format::R11G11B10_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT3 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT3;
		break;
	case Format::R16G16B16A16_FLOAT:
	case Format::R32G32B32A32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT4 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4;
		break;
	default:
		assert(0); // implement format!
		break;
	}
	device->BindComputeShader(&shaders[cs], cmd);
	
	// Horizontal:
	{
		const TextureDesc& desc = temp.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 1;
		postprocess.params0.y = 0;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd, mip_src);
		device->BindUAV(&temp, 0, cmd, mip_dst);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Vertical:
	{
		const TextureDesc& desc = output.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&temp, 0, cmd, mip_dst); // <- also mip_dst because it's second pass!
		device->BindUAV(&output, 0, cmd, mip_dst);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	device->EventEnd(cmd);
}
void Postprocess_Blur_Bilateral(
	const Texture& input,
	const Texture& lineardepth,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	float depth_threshold,
	int mip_src,
	int mip_dst,
	bool wide
)
{
	device->EventBegin("Postprocess_Blur_Bilateral", cmd);

	SHADERTYPE cs = CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4;
	switch (output.GetDesc().format)
	{
	case Format::R16_UNORM:
	case Format::R8_UNORM:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_UNORM1 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_UNORM1;
		break;
	case Format::R16_FLOAT:
	case Format::R32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT1 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT1;
		break;
	case Format::R16G16B16A16_UNORM:
	case Format::R8G8B8A8_UNORM:
	case Format::B8G8R8A8_UNORM:
	case Format::R10G10B10A2_UNORM:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_UNORM4 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_UNORM4;
		break;
	case Format::R11G11B10_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT3 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT3;
		break;
	case Format::R16G16B16A16_FLOAT:
	case Format::R32G32B32A32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT4 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4;
		break;
	default:
		assert(0); // implement format!
		break;
	}
	device->BindComputeShader(&shaders[cs], cmd);

	// Horizontal:
	{
		const TextureDesc& desc = temp.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 1;
		postprocess.params0.y = 0;
		postprocess.params0.w = depth_threshold;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd, mip_src);
		device->BindUAV(&temp, 0, cmd, mip_dst);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Vertical:
	{
		const TextureDesc& desc = output.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		postprocess.params0.w = depth_threshold;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&temp, 0, cmd, mip_dst); // <- also mip_dst because it's second pass!
		device->BindUAV(&output, 0, cmd, mip_dst);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout, mip_dst),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	device->EventEnd(cmd);
}
void CreateSSAOResources(SSAOResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.format = Format::R8_UNORM;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.temp);
}
void Postprocess_SSAO(
	const SSAOResources& res,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float range,
	uint32_t samplecount,
	float power
)
{
	device->EventBegin("Postprocess_SSAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("SSAO", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSAO], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	ssao_range = range;
	ssao_samplecount = (float)samplecount;
	ssao_power = power;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	Postprocess_Blur_Bilateral(output, lineardepth, res.temp, output, cmd, 1.2f, -1, -1, true);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void Postprocess_HBAO(
	const SSAOResources& res,
	const CameraComponent& camera,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float power
)
{
	device->EventBegin("Postprocess_HBAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("HBAO", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_HBAO], cmd);


	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = 1;
	postprocess.params0.y = 0;
	hbao_power = power;

	// Load first element of projection matrix which is the cotangent of the horizontal FOV divided by 2.
	const float TanHalfFovH = 1.0f / camera.Projection.m[0][0];
	const float FocalLenX = 1.0f / TanHalfFovH * ((float)postprocess.resolution.y / (float)postprocess.resolution.x);
	const float FocalLenY = 1.0f / TanHalfFovH;
	const float InvFocalLenX = 1.0f / FocalLenX;
	const float InvFocalLenY = 1.0f / FocalLenY;
	const float UVToViewAX = 2.0f * InvFocalLenX;
	const float UVToViewAY = -2.0f * InvFocalLenY;
	const float UVToViewBX = -1.0f * InvFocalLenX;
	const float UVToViewBY = 1.0f * InvFocalLenY;
	postprocess.params1.x = UVToViewAX;
	postprocess.params1.y = UVToViewAY;
	postprocess.params1.z = UVToViewBX;
	postprocess.params1.w = UVToViewBY;

	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	// horizontal pass:
	{
		device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
		const GPUResource* uavs[] = {
			&res.temp,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.temp, res.temp.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_HBAO_THREADCOUNT - 1) / POSTPROCESS_HBAO_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
			);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.temp, ResourceState::UNORDERED_ACCESS, res.temp.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// vertical pass:
	{
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.temp, 0, cmd);
		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_HBAO_THREADCOUNT - 1) / POSTPROCESS_HBAO_THREADCOUNT,
			1,
			cmd
			);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	Postprocess_Blur_Bilateral(output, lineardepth, res.temp, output, cmd, 1.2f, -1, -1, true);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateMSAOResources(MSAOResources& res, XMUINT2 resolution)
{
	TextureDesc saved_desc;
	saved_desc.format = Format::R32_FLOAT;
	saved_desc.width = resolution.x;
	saved_desc.height = resolution.y;
	saved_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	saved_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	const uint32_t bufferWidth = saved_desc.width;
	const uint32_t bufferWidth1 = (bufferWidth + 1) / 2;
	const uint32_t bufferWidth2 = (bufferWidth + 3) / 4;
	const uint32_t bufferWidth3 = (bufferWidth + 7) / 8;
	const uint32_t bufferWidth4 = (bufferWidth + 15) / 16;
	const uint32_t bufferWidth5 = (bufferWidth + 31) / 32;
	const uint32_t bufferWidth6 = (bufferWidth + 63) / 64;
	const uint32_t bufferHeight = saved_desc.height;
	const uint32_t bufferHeight1 = (bufferHeight + 1) / 2;
	const uint32_t bufferHeight2 = (bufferHeight + 3) / 4;
	const uint32_t bufferHeight3 = (bufferHeight + 7) / 8;
	const uint32_t bufferHeight4 = (bufferHeight + 15) / 16;
	const uint32_t bufferHeight5 = (bufferHeight + 31) / 32;
	const uint32_t bufferHeight6 = (bufferHeight + 63) / 64;

	TextureDesc desc = saved_desc;
	desc.width = bufferWidth1;
	desc.height = bufferHeight1;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize1);
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled1);

	desc = saved_desc;
	desc.width = bufferWidth2;
	desc.height = bufferHeight2;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize2);
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled2);

	desc = saved_desc;
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize3);
	desc.width = bufferWidth5;
	desc.height = bufferHeight5;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled3);

	desc = saved_desc;
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize4);
	desc.width = bufferWidth6;
	desc.height = bufferHeight6;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled4);

	desc = saved_desc;
	desc.format = Format::R8_UNORM;
	desc.width = bufferWidth1;
	desc.height = bufferHeight1;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged1);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq1);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth1);
	desc.width = bufferWidth2;
	desc.height = bufferHeight2;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged2);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq2);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth2);
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged3);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq3);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth3);
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged4);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq4);
}
void Postprocess_MSAO(
	const MSAOResources& res,
	const CameraComponent& camera,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float power
	)
{
	device->EventBegin("Postprocess_MSAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("MSAO", cmd);

	// Depth downsampling + deinterleaving pass1:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS1], cmd);

		const GPUResource* uavs[] = {
			&res.texture_lineardepth_downsize1,
			&res.texture_lineardepth_tiled1,
			&res.texture_lineardepth_downsize2,
			&res.texture_lineardepth_tiled2,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize1, res.texture_lineardepth_downsize1.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled1, res.texture_lineardepth_tiled1.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_downsize2, res.texture_lineardepth_downsize2.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled2, res.texture_lineardepth_tiled2.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const TextureDesc& desc = res.texture_lineardepth_tiled2.GetDesc();
		device->Dispatch(desc.width, desc.height, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_lineardepth_downsize1, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize1.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled1, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled1.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_downsize2, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize2.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled2, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled2.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Depth downsampling + deinterleaving pass2:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS2], cmd);

		device->BindResource(&res.texture_lineardepth_downsize2, 0, cmd);

		const GPUResource* uavs[] = {
			&res.texture_lineardepth_downsize3,
			&res.texture_lineardepth_tiled3,
			&res.texture_lineardepth_downsize4,
			&res.texture_lineardepth_tiled4,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize3, res.texture_lineardepth_downsize3.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled3, res.texture_lineardepth_tiled3.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_downsize4, res.texture_lineardepth_downsize4.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled4, res.texture_lineardepth_tiled4.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const TextureDesc& desc = res.texture_lineardepth_tiled4.GetDesc();
		device->Dispatch(desc.width, desc.height, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_lineardepth_downsize3, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize3.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled3, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled3.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_downsize4, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize4.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled4, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled4.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}


	float SampleThickness[12];
	SampleThickness[0] = sqrt(1.0f - 0.2f * 0.2f);
	SampleThickness[1] = sqrt(1.0f - 0.4f * 0.4f);
	SampleThickness[2] = sqrt(1.0f - 0.6f * 0.6f);
	SampleThickness[3] = sqrt(1.0f - 0.8f * 0.8f);
	SampleThickness[4] = sqrt(1.0f - 0.2f * 0.2f - 0.2f * 0.2f);
	SampleThickness[5] = sqrt(1.0f - 0.2f * 0.2f - 0.4f * 0.4f);
	SampleThickness[6] = sqrt(1.0f - 0.2f * 0.2f - 0.6f * 0.6f);
	SampleThickness[7] = sqrt(1.0f - 0.2f * 0.2f - 0.8f * 0.8f);
	SampleThickness[8] = sqrt(1.0f - 0.4f * 0.4f - 0.4f * 0.4f);
	SampleThickness[9] = sqrt(1.0f - 0.4f * 0.4f - 0.6f * 0.6f);
	SampleThickness[10] = sqrt(1.0f - 0.4f * 0.4f - 0.8f * 0.8f);
	SampleThickness[11] = sqrt(1.0f - 0.6f * 0.6f - 0.6f * 0.6f);
	static float RejectionFalloff = 2.0f;
	const float Accentuation = 0.1f * power;

	// The msao_compute will be called repeatedly, so create a local lambda for it:
	auto msao_compute = [&](const Texture& write_result, const Texture& read_depth) 
	{
		const TextureDesc& desc = read_depth.GetDesc();

		MSAO msao;

		// Load first element of projection matrix which is the cotangent of the horizontal FOV divided by 2.
		const float TanHalfFovH = 1.0f / camera.Projection.m[0][0];

		// Here we compute multipliers that convert the center depth value into (the reciprocal of)
		// sphere thicknesses at each sample location.  This assumes a maximum sample radius of 5
		// units, but since a sphere has no thickness at its extent, we don't need to sample that far
		// out.  Only samples whole integer offsets with distance less than 25 are used.  This means
		// that there is no sample at (3, 4) because its distance is exactly 25 (and has a thickness of 0.)

		// The shaders are set up to sample a circular region within a 5-pixel radius.
		const float ScreenspaceDiameter = 10.0f;

		// SphereDiameter = CenterDepth * ThicknessMultiplier.  This will compute the thickness of a sphere centered
		// at a specific depth.  The ellipsoid scale can stretch a sphere into an ellipsoid, which changes the
		// characteristics of the AO.
		// TanHalfFovH:  Radius of sphere in depth units if its center lies at Z = 1
		// ScreenspaceDiameter:  Diameter of sample sphere in pixel units
		// ScreenspaceDiameter / BufferWidth:  Ratio of the screen width that the sphere actually covers
		// Note about the "2.0f * ":  Diameter = 2 * Radius
		float ThicknessMultiplier = 2.0f * TanHalfFovH * ScreenspaceDiameter / desc.width;
		if (desc.array_size == 1)
		{
			ThicknessMultiplier *= 2.0f;
		}

		// This will transform a depth value from [0, thickness] to [0, 1].
		float InverseRangeFactor = 1.0f / ThicknessMultiplier;

		// The thicknesses are smaller for all off-center samples of the sphere.  Compute thicknesses relative
		// to the center sample.
		msao.xInvThicknessTable[0].x = InverseRangeFactor / SampleThickness[0];
		msao.xInvThicknessTable[0].y = InverseRangeFactor / SampleThickness[1];
		msao.xInvThicknessTable[0].z = InverseRangeFactor / SampleThickness[2];
		msao.xInvThicknessTable[0].w = InverseRangeFactor / SampleThickness[3];
		msao.xInvThicknessTable[1].x = InverseRangeFactor / SampleThickness[4];
		msao.xInvThicknessTable[1].y = InverseRangeFactor / SampleThickness[5];
		msao.xInvThicknessTable[1].z = InverseRangeFactor / SampleThickness[6];
		msao.xInvThicknessTable[1].w = InverseRangeFactor / SampleThickness[7];
		msao.xInvThicknessTable[2].x = InverseRangeFactor / SampleThickness[8];
		msao.xInvThicknessTable[2].y = InverseRangeFactor / SampleThickness[9];
		msao.xInvThicknessTable[2].z = InverseRangeFactor / SampleThickness[10];
		msao.xInvThicknessTable[2].w = InverseRangeFactor / SampleThickness[11];

		// These are the weights that are multiplied against the samples because not all samples are
		// equally important.  The farther the sample is from the center location, the less they matter.
		// We use the thickness of the sphere to determine the weight.  The scalars in front are the number
		// of samples with this weight because we sum the samples together before multiplying by the weight,
		// so as an aggregate all of those samples matter more.  After generating this table, the weights
		// are normalized.
		msao.xSampleWeightTable[0].x = 4.0f * SampleThickness[0];    // Axial
		msao.xSampleWeightTable[0].y = 4.0f * SampleThickness[1];    // Axial
		msao.xSampleWeightTable[0].z = 4.0f * SampleThickness[2];    // Axial
		msao.xSampleWeightTable[0].w = 4.0f * SampleThickness[3];    // Axial
		msao.xSampleWeightTable[1].x = 4.0f * SampleThickness[4];    // Diagonal
		msao.xSampleWeightTable[1].y = 8.0f * SampleThickness[5];    // L-shaped
		msao.xSampleWeightTable[1].z = 8.0f * SampleThickness[6];    // L-shaped
		msao.xSampleWeightTable[1].w = 8.0f * SampleThickness[7];    // L-shaped
		msao.xSampleWeightTable[2].x = 4.0f * SampleThickness[8];    // Diagonal
		msao.xSampleWeightTable[2].y = 8.0f * SampleThickness[9];    // L-shaped
		msao.xSampleWeightTable[2].z = 8.0f * SampleThickness[10];   // L-shaped
		msao.xSampleWeightTable[2].w = 4.0f * SampleThickness[11];   // Diagonal

		// If we aren't using all of the samples, delete their weights before we normalize.
#ifndef MSAO_SAMPLE_EXHAUSTIVELY
		msao.xSampleWeightTable[0].x = 0.0f;
		msao.xSampleWeightTable[0].z = 0.0f;
		msao.xSampleWeightTable[1].y = 0.0f;
		msao.xSampleWeightTable[1].w = 0.0f;
		msao.xSampleWeightTable[2].y = 0.0f;
#endif

		// Normalize the weights by dividing by the sum of all weights
		float totalWeight = 0.0f;
		totalWeight += msao.xSampleWeightTable[0].x;
		totalWeight += msao.xSampleWeightTable[0].y;
		totalWeight += msao.xSampleWeightTable[0].z;
		totalWeight += msao.xSampleWeightTable[0].w;
		totalWeight += msao.xSampleWeightTable[1].x;
		totalWeight += msao.xSampleWeightTable[1].y;
		totalWeight += msao.xSampleWeightTable[1].z;
		totalWeight += msao.xSampleWeightTable[1].w;
		totalWeight += msao.xSampleWeightTable[2].x;
		totalWeight += msao.xSampleWeightTable[2].y;
		totalWeight += msao.xSampleWeightTable[2].z;
		totalWeight += msao.xSampleWeightTable[2].w;
		msao.xSampleWeightTable[0].x /= totalWeight;
		msao.xSampleWeightTable[0].y /= totalWeight;
		msao.xSampleWeightTable[0].z /= totalWeight;
		msao.xSampleWeightTable[0].w /= totalWeight;
		msao.xSampleWeightTable[1].x /= totalWeight;
		msao.xSampleWeightTable[1].y /= totalWeight;
		msao.xSampleWeightTable[1].z /= totalWeight;
		msao.xSampleWeightTable[1].w /= totalWeight;
		msao.xSampleWeightTable[2].x /= totalWeight;
		msao.xSampleWeightTable[2].y /= totalWeight;
		msao.xSampleWeightTable[2].z /= totalWeight;
		msao.xSampleWeightTable[2].w /= totalWeight;

		msao.xInvSliceDimension.x = 1.0f / desc.width;
		msao.xInvSliceDimension.y = 1.0f / desc.height;
		msao.xRejectFadeoff = 1.0f / -RejectionFalloff;
		msao.xRcpAccentuation = 1.0f / (1.0f + Accentuation);

		device->BindDynamicConstantBuffer(msao, CBSLOT_MSAO, cmd);

		device->BindResource(&read_depth, 0, cmd);

		const GPUResource* uavs[] = {
			&write_result,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&write_result, write_result.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		if (desc.array_size == 1)
		{
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO], cmd);
			device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);
		}
		else
		{
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_INTERLEAVE], cmd);
			device->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, desc.array_size, cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&write_result, ResourceState::UNORDERED_ACCESS, write_result.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}; // end of lambda: msao_compute

	msao_compute(res.texture_ao_merged4, res.texture_lineardepth_tiled4);
	msao_compute(res.texture_ao_hq4, res.texture_lineardepth_downsize4);

	msao_compute(res.texture_ao_merged3, res.texture_lineardepth_tiled3);
	msao_compute(res.texture_ao_hq3, res.texture_lineardepth_downsize3);

	msao_compute(res.texture_ao_merged2, res.texture_lineardepth_tiled2);
	msao_compute(res.texture_ao_hq2, res.texture_lineardepth_downsize2);

	msao_compute(res.texture_ao_merged1, res.texture_lineardepth_tiled1);
	msao_compute(res.texture_ao_hq1, res.texture_lineardepth_downsize1);

	auto blur_and_upsample = [&](const Texture& Destination, const Texture& HiResDepth, const Texture& LoResDepth,
		const Texture* InterleavedAO, const Texture* HighQualityAO, const Texture* HiResAO)
	{
		const uint32_t LoWidth = LoResDepth.GetDesc().width;
		const uint32_t LoHeight = LoResDepth.GetDesc().height;
		const uint32_t HiWidth = HiResDepth.GetDesc().width;
		const uint32_t HiHeight = HiResDepth.GetDesc().height;

		if (HiResAO == nullptr)
		{
			if (HighQualityAO == nullptr)
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE], cmd);
			}
			else
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN], cmd);
			}
		}
		else
		{
			if (HighQualityAO == nullptr)
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_BLENDOUT], cmd);
			}
			else
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN_BLENDOUT], cmd);
			}
		}

		static float g_NoiseFilterTolerance = -3.0f;
		static float g_BlurTolerance = -5.0f;
		static float g_UpsampleTolerance = -7.0f;

		MSAO_UPSAMPLE msao_upsample;
		msao_upsample.InvLowResolution = float2(1.0f / LoWidth, 1.0f / LoHeight);
		msao_upsample.InvHighResolution = float2(1.0f / HiWidth, 1.0f / HiHeight);
		msao_upsample.kBlurTolerance = 1.0f - powf(10.0f, g_BlurTolerance) * 1920.0f / (float)LoWidth;
		msao_upsample.kBlurTolerance *= msao_upsample.kBlurTolerance;
		msao_upsample.kUpsampleTolerance = powf(10.0f, g_UpsampleTolerance);
		msao_upsample.NoiseFilterStrength = 1.0f / (powf(10.0f, g_NoiseFilterTolerance) + msao_upsample.kUpsampleTolerance);
		msao_upsample.StepSize = (float)lineardepth.GetDesc().width / (float)LoWidth;
		device->PushConstants(&msao_upsample, sizeof(msao_upsample), cmd);
		
		device->BindUAV(&Destination, 0, cmd);
		device->BindResource(&LoResDepth, 0, cmd);
		device->BindResource(&HiResDepth, 1, cmd);
		if (InterleavedAO != nullptr)
		{
			device->BindResource(InterleavedAO, 2, cmd);
		}
		if (HighQualityAO != nullptr)
		{
			device->BindResource(HighQualityAO, 3, cmd);
		}
		if (HiResAO != nullptr)
		{
			device->BindResource(HiResAO, 4, cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&Destination, Destination.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((HiWidth + 2 + 15) / 16, (HiHeight + 2 + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&Destination, ResourceState::UNORDERED_ACCESS, Destination.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	};

	blur_and_upsample(
		res.texture_ao_smooth3,
		res.texture_lineardepth_downsize3,
		res.texture_lineardepth_downsize4,
		&res.texture_ao_merged4,
		&res.texture_ao_hq4,
		&res.texture_ao_merged3
	);

	blur_and_upsample(
		res.texture_ao_smooth2,
		res.texture_lineardepth_downsize2,
		res.texture_lineardepth_downsize3,
		&res.texture_ao_smooth3,
		&res.texture_ao_hq3,
		&res.texture_ao_merged2
	);

	blur_and_upsample(
		res.texture_ao_smooth1,
		res.texture_lineardepth_downsize1,
		res.texture_lineardepth_downsize2,
		&res.texture_ao_smooth2,
		&res.texture_ao_hq2,
		&res.texture_ao_merged1
	);

	blur_and_upsample(
		output,
		lineardepth,
		res.texture_lineardepth_downsize1,
		&res.texture_ao_smooth1,
		&res.texture_ao_hq1,
		nullptr
	);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateRTAOResources(RTAOResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.normals);
	device->SetName(&res.normals, "rtao_normals");

	GPUBufferDesc bd;
	bd.stride = sizeof(uint);
	bd.size = bd.stride *
		((desc.width + 7) / 8) *
		((desc.height + 3) / 4);
	bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateBuffer(&bd, nullptr, &res.tiles);
	device->SetName(&res.tiles, "rtshadow_tiles");
	device->CreateBuffer(&bd, nullptr, &res.metadata);
	device->SetName(&res.metadata, "rtshadow_metadata");

	desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.scratch[0]);
	device->SetName(&res.scratch[0], "rtshadow_scratch[0]");
	device->CreateTexture(&desc, nullptr, &res.scratch[1]);
	device->SetName(&res.scratch[1], "rtshadow_scratch[1]");

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.moments[0]);
	device->SetName(&res.moments[0], "rtshadow_moments[0]");
	device->CreateTexture(&desc, nullptr, &res.moments[1]);
	device->SetName(&res.moments[1], "rtshadow_moments[1]");
}
void Postprocess_RTAO(
	const RTAOResources& res,
	const Scene& scene,
	const Texture& output,
	CommandList cmd,
	float range,
	float power,
	uint8_t instanceInclusionMask
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid())
		return;

	device->EventBegin("Postprocess_RTAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("RTAO", cmd);

	if (res.frame == 0)
	{
		// Maybe we don't need to clear them all, but it's safer this way:
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1], res.scratch[1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0], res.moments[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1], res.moments[1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&res.normals, 0, cmd);
		device->ClearUAV(&res.scratch[0], 0, cmd);
		device->ClearUAV(&res.scratch[1], 0, cmd);
		device->ClearUAV(&res.moments[0], 0, cmd);
		device->ClearUAV(&res.moments[1], 0, cmd);
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	BindCommonResources(cmd);

	device->EventBegin("Raytrace", cmd);

	const TextureDesc& desc = output.GetDesc();

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO], cmd);

	const GPUResource* uavs[] = {
		&output,
		&res.normals,
		&res.tiles
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	rtao_range = range;
	rtao_power = power;
	postprocess.params0.w = (float)res.frame;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.tiles, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&res.normals, ResourceState::UNORDERED_ACCESS, res.normals.desc.layout),
			GPUBarrier::Buffer(&res.tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);

	int temporal_output = res.frame % 2;
	int temporal_history = 1 - temporal_output;

	// Denoise - Tile Classification:
	{
		device->EventBegin("Denoise - Tile Classification", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_TILECLASSIFICATION], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.tiles, 1, cmd);
		device->BindResource(&res.moments[temporal_history], 2, cmd);
		device->BindResource(&res.scratch[1], 3, cmd);

		const GPUResource* uavs[] = {
			&res.scratch[0],
			&res.moments[temporal_output],
			&res.metadata
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[temporal_output], res.moments[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Buffer(&res.metadata, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.scratch[0], ResourceState::UNORDERED_ACCESS, res.scratch[0].desc.layout),
				GPUBarrier::Image(&res.moments[temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[temporal_output].desc.layout),
				GPUBarrier::Buffer(&res.metadata, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Denoise - Spatial filtering:
	{
		device->EventBegin("Denoise - Filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_FILTER], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.metadata, 1, cmd);

		// pass0:
		{
			device->BindResource(&res.scratch[0], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[1],
				&output
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[1], res.scratch[1].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 0;
			postprocess.params1.y = 1;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		// pass1:
		{
			device->BindResource(&res.scratch[1], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[0],
				&output
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.scratch[1], ResourceState::UNORDERED_ACCESS, res.scratch[1].desc.layout),
					GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 1;
			postprocess.params1.y = 2;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		// pass2:
		{
			device->BindResource(&res.scratch[0], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[1],
				&output
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.scratch[0], ResourceState::UNORDERED_ACCESS, res.scratch[0].desc.layout),
					GPUBarrier::Image(&res.scratch[1], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 2;
			postprocess.params1.y = 4;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.scratch[1], ResourceState::UNORDERED_ACCESS, res.scratch[1].desc.layout),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}
	res.frame++;

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateRTReflectionResources(RTReflectionResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayIndirectSpecular);
	device->CreateTexture(&desc, nullptr, &res.texture_rayDirectionPDF);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayLengths);
	device->SetName(&res.texture_rayLengths, "ssr_rayLengths");

	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	device->CreateTexture(&desc, nullptr, &res.texture_bilateral_temp);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_variance);
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_reprojectionDepth);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[1]);
}
void Postprocess_RTReflection(
	const RTReflectionResources& res,
	const Scene& scene,
	const Texture& output,
	CommandList cmd,
	float range,
	uint8_t instanceInclusionMask
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	device->EventBegin("Postprocess_RTReflection", cmd);
	auto profilerRange = wi::profiler::BeginRangeGPU("RTReflection", cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = output.desc;

	// Render half-res:
	PostProcess postprocess;
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	rtreflection_range = range;
	rtreflection_frame = (float)res.frame;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));

	{
		//device->EventBegin("RTReflection Raytrace pass", cmd);

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
		device->BindRaytracingPipelineState(&RTPSO_reflection, cmd);
#else
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTREFLECTION], cmd);
#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, res.texture_rayIndirectSpecular.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, res.texture_rayDirectionPDF.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayLengths, res.texture_rayLengths.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
		size_t shaderIdentifierSize = device->GetShaderIdentifierSize();
		GraphicsDevice::GPUAllocation shadertable_raygen = device->AllocateGPU(shaderIdentifierSize, cmd);
		GraphicsDevice::GPUAllocation shadertable_miss = device->AllocateGPU(shaderIdentifierSize, cmd);
		GraphicsDevice::GPUAllocation shadertable_hitgroup = device->AllocateGPU(shaderIdentifierSize, cmd);

		device->WriteShaderIdentifier(&RTPSO_reflection, 0, shadertable_raygen.data);
		device->WriteShaderIdentifier(&RTPSO_reflection, 1, shadertable_miss.data);
		device->WriteShaderIdentifier(&RTPSO_reflection, 2, shadertable_hitgroup.data);

		DispatchRaysDesc dispatchraysdesc;
		dispatchraysdesc.ray_generation.buffer = &shadertable_raygen.buffer;
		dispatchraysdesc.ray_generation.offset = shadertable_raygen.offset;
		dispatchraysdesc.ray_generation.size = shaderIdentifierSize;

		dispatchraysdesc.miss.buffer = &shadertable_miss.buffer;
		dispatchraysdesc.miss.offset = shadertable_miss.offset;
		dispatchraysdesc.miss.size = shaderIdentifierSize;
		dispatchraysdesc.miss.stride = shaderIdentifierSize;

		dispatchraysdesc.hit_group.buffer = &shadertable_hitgroup.buffer;
		dispatchraysdesc.hit_group.offset = shadertable_hitgroup.offset;
		dispatchraysdesc.hit_group.size = shaderIdentifierSize;
		dispatchraysdesc.hit_group.stride = shaderIdentifierSize;

		dispatchraysdesc.width = desc.width / 2;
		dispatchraysdesc.height = desc.height / 2;

		device->DispatchRays(&dispatchraysdesc, cmd);

#else

		device->Dispatch(
			(res.texture_rayIndirectSpecular.GetDesc().width + 7) / 8,
			(res.texture_rayIndirectSpecular.GetDesc().height + 3) / 4,
			1,
			cmd
		);

#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, ResourceState::UNORDERED_ACCESS, res.texture_rayIndirectSpecular.desc.layout),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, ResourceState::UNORDERED_ACCESS, res.texture_rayDirectionPDF.desc.layout),
				GPUBarrier::Image(&res.texture_rayLengths, ResourceState::UNORDERED_ACCESS, res.texture_rayLengths.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		//device->EventEnd(cmd);
	}

	// Upscale to full-res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	// Resolve pass:
	{
		device->EventBegin("RTReflection Resolve pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], cmd);

		const GPUResource* resarray[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_resolve,
			&res.texture_resolve_variance,
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_resolve, res.texture_resolve.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_variance, res.texture_resolve_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, res.texture_resolve_reprojectionDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_resolve.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_resolve.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_resolve, ResourceState::UNORDERED_ACCESS, res.texture_resolve.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_variance, ResourceState::UNORDERED_ACCESS, res.texture_resolve_variance.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, ResourceState::UNORDERED_ACCESS, res.texture_resolve_reprojectionDepth.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	int temporal_output = device->GetFrameCount() % 2;
	int temporal_history = 1 - temporal_output;

	// Temporal pass:
	{
		device->EventBegin("RTReflection Temporal pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_resolve,
			&res.texture_temporal[temporal_history],
			&res.texture_resolve_variance,
			&res.texture_temporal_variance[temporal_history],
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Bilateral blur pass:
	{
		device->EventBegin("RTReflection Bilateral blur pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_BILATERAL], cmd);

		// Horizontal:
		{
			postprocess.params0.x = 1;
			postprocess.params0.y = 0;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			const GPUResource* resarray[] = {
				&res.texture_temporal[temporal_output],
				&res.texture_temporal_variance[temporal_output],
			};
			device->BindResources(resarray, 0, arraysize(resarray), cmd);

			const GPUResource* uavs[] = {
				&res.texture_bilateral_temp,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_bilateral_temp, res.texture_bilateral_temp.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(res.texture_bilateral_temp.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(res.texture_bilateral_temp.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.texture_bilateral_temp, ResourceState::UNORDERED_ACCESS, res.texture_bilateral_temp.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		// Vertical:
		{
			postprocess.params0.x = 0;
			postprocess.params0.y = 1;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			const GPUResource* resarray[] = {
				&res.texture_bilateral_temp,
				&res.texture_temporal_variance[temporal_output],
			};
			device->BindResources(resarray, 0, arraysize(resarray), cmd);

			const GPUResource* uavs[] = {
				&output,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(profilerRange);
	device->EventEnd(cmd);
}
void CreateSSRResources(SSRResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.width = (resolution.x + SSR_TILESIZE - 1) / SSR_TILESIZE;
	tile_desc.height = (resolution.y + SSR_TILESIZE - 1) / SSR_TILESIZE;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	tile_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tile_minmax_roughness);

	tile_desc.height = resolution.y;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tile_minmax_roughness_horizontal);

	GPUBufferDesc bufferdesc;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	bufferdesc.size = TILE_STATISTICS_CAPACITY * sizeof(uint);
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_RAW | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_tracing_statistics);

	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_desc.width * tile_desc.height * bufferdesc.stride;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_expensive);

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.format = Format::R16G16B16A16_FLOAT;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.texture_rayIndirectSpecular);
	device->CreateTexture(&desc, nullptr, &res.texture_rayDirectionPDF);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayLengths);
	device->SetName(&res.texture_rayLengths, "ssr_rayLengths");

	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	device->CreateTexture(&desc, nullptr, &res.texture_bilateral_temp);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_variance);
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_reprojectionDepth);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[1]);

	desc.width = (uint32_t)std::pow(2.0f, 1.0f + std::floor(std::log2((float)resolution.x / 2)));
	desc.height = (uint32_t)std::pow(2.0f, 1.0f + std::floor(std::log2((float)resolution.y / 2)));
	desc.format = Format::R32G32_FLOAT;
	desc.mip_levels = 1 + (uint32_t)std::floor(std::log2f(std::max((float)desc.width, (float)desc.height)));
	device->CreateTexture(&desc, nullptr, &res.texture_depth_hierarchy);

	for (uint32_t i = 0; i < desc.mip_levels; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&res.texture_depth_hierarchy, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_depth_hierarchy, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}
}
void Postprocess_SSR(
	const SSRResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_SSR", cmd);

	auto range = wi::profiler::BeginRangeGPU("SSR", cmd);

	BindCommonResources(cmd);

	// Compute tile classification (horizontal):
	{
		device->EventBegin("SSR Tile Classification - Horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_HORIZONTAL], cmd);

		const GPUResource* uavs[] = {
			&res.texture_tile_minmax_roughness_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tile_minmax_roughness_horizontal, res.texture_tile_minmax_roughness_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tile_minmax_roughness_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tile_minmax_roughness_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tile_minmax_roughness_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tile_minmax_roughness_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile classification (vertical):
	{
		device->EventBegin("SSR Tile Classification - Vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_VERTICAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tile_minmax_roughness_horizontal,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_tracing_statistics,
			&res.buffer_tiles_tracing_earlyexit,
			&res.buffer_tiles_tracing_cheap,
			&res.buffer_tiles_tracing_expensive,
			&res.texture_tile_minmax_roughness,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tile_minmax_roughness, res.texture_tile_minmax_roughness.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tile_minmax_roughness.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tile_minmax_roughness.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tile_minmax_roughness, ResourceState::UNORDERED_ACCESS, res.texture_tile_minmax_roughness.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Kick indirect tile jobs:
	{
		device->EventBegin("SSR Kickjobs", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_KICKJOBS], cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_tracing_statistics,
			&res.buffer_tiles_tracing_earlyexit,
			&res.buffer_tiles_tracing_cheap,
			&res.buffer_tiles_tracing_expensive,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(1, 1, 1, cmd);

		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.buffer_tile_tracing_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->EventEnd(cmd);
	}

	PostProcess postprocess;

	// Depth hierarchy:
	{
		device->EventBegin("SSR Depth hierarchy pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_DEPTHHIERARCHY], cmd);

		TextureDesc hierarchyDesc = res.texture_depth_hierarchy.GetDesc();

		{
			device->BindUAV(&res.texture_depth_hierarchy, 0, cmd, 0);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, res.texture_depth_hierarchy.desc.layout, ResourceState::UNORDERED_ACCESS, 0),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params0.x = (float)hierarchyDesc.width;
			postprocess.params0.y = (float)hierarchyDesc.height;
			postprocess.params0.z = 1.0f;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				std::max(1u, hierarchyDesc.width / POSTPROCESS_BLOCKSIZE),
				std::max(1u, hierarchyDesc.height / POSTPROCESS_BLOCKSIZE),
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.texture_depth_hierarchy, ResourceState::UNORDERED_ACCESS, res.texture_depth_hierarchy.desc.layout, 0),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		for (uint32_t i = 1; i < hierarchyDesc.mip_levels; i++)
		{
			device->BindResource(&res.texture_depth_hierarchy, 0, cmd, i - 1);
			device->BindUAV(&res.texture_depth_hierarchy, 0, cmd, i);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, res.texture_depth_hierarchy.desc.layout, ResourceState::UNORDERED_ACCESS, i),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			hierarchyDesc.width /= 2;
			hierarchyDesc.height /= 2;

			hierarchyDesc.width = std::max(1u, hierarchyDesc.width);
			hierarchyDesc.height = std::max(1u, hierarchyDesc.height);

			postprocess.params0.x = (float)hierarchyDesc.width;
			postprocess.params0.y = (float)hierarchyDesc.height;
			postprocess.params0.z = 0.0f;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				std::max(1u, hierarchyDesc.width / POSTPROCESS_BLOCKSIZE),
				std::max(1u, hierarchyDesc.height / POSTPROCESS_BLOCKSIZE),
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.texture_depth_hierarchy, ResourceState::UNORDERED_ACCESS, res.texture_depth_hierarchy.desc.layout, i),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->EventEnd(cmd);
	}

	const TextureDesc& desc = output.GetDesc();

	// Render half-res:
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Factor to scale ratio between hierarchy and trace pass
	postprocess.params1.x = (float)postprocess.resolution.x / (float)res.texture_depth_hierarchy.GetDesc().width;
	postprocess.params1.y = (float)postprocess.resolution.y / (float)res.texture_depth_hierarchy.GetDesc().height;
	postprocess.params1.z = 1.0f / postprocess.params1.x;
	postprocess.params1.w = 1.0f / postprocess.params1.y;
	ssr_frame = (float)res.frame;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	// Raytrace pass:
	{
		device->EventBegin("SSR Raytrace pass", cmd);

		const GPUResource* resarray[] = {
			&res.texture_depth_hierarchy,
			&input,
			&res.buffer_tiles_tracing_earlyexit,
			&res.buffer_tiles_tracing_cheap,
			&res.buffer_tiles_tracing_expensive
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, res.texture_rayIndirectSpecular.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, res.texture_rayDirectionPDF.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayLengths, res.texture_rayLengths.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_EARLYEXIT], cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, INDIRECT_OFFSET_EARLYEXIT, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_CHEAP], cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, INDIRECT_OFFSET_CHEAP, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE], cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, INDIRECT_OFFSET_EXPENSIVE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, ResourceState::UNORDERED_ACCESS, res.texture_rayIndirectSpecular.desc.layout),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, ResourceState::UNORDERED_ACCESS, res.texture_rayDirectionPDF.desc.layout),
				GPUBarrier::Image(&res.texture_rayLengths, ResourceState::UNORDERED_ACCESS, res.texture_rayLengths.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Upscale to full-res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	// Resolve pass:
	{
		device->EventBegin("SSR Resolve pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], cmd);

		const GPUResource* resarray[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_resolve,
			&res.texture_resolve_variance,
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_resolve, res.texture_resolve.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_variance, res.texture_resolve_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, res.texture_resolve_reprojectionDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_resolve.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_resolve.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_resolve, ResourceState::UNORDERED_ACCESS, res.texture_resolve.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_variance, ResourceState::UNORDERED_ACCESS, res.texture_resolve_variance.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, ResourceState::UNORDERED_ACCESS, res.texture_resolve_reprojectionDepth.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	int temporal_output = device->GetFrameCount() % 2;
	int temporal_history = 1 - temporal_output;

	// Temporal pass:
	{
		device->EventBegin("SSR Temporal pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_resolve,
			&res.texture_temporal[temporal_history],
			&res.texture_resolve_variance,
			&res.texture_temporal_variance[temporal_history],
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Bilateral blur pass:
	{
		device->EventBegin("SSR Bilateral blur pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_BILATERAL], cmd);

		// Horizontal:
		{
			postprocess.params0.x = 1;
			postprocess.params0.y = 0;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			const GPUResource* resarray[] = {
				&res.texture_temporal[temporal_output],
				&res.texture_temporal_variance[temporal_output],
			};
			device->BindResources(resarray, 0, arraysize(resarray), cmd);

			const GPUResource* uavs[] = {
				&res.texture_bilateral_temp,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_bilateral_temp, res.texture_bilateral_temp.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(res.texture_bilateral_temp.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(res.texture_bilateral_temp.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.texture_bilateral_temp, ResourceState::UNORDERED_ACCESS, res.texture_bilateral_temp.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		// Vertical:
		{
			postprocess.params0.x = 0;
			postprocess.params0.y = 1;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			const GPUResource* resarray[] = {
				&res.texture_bilateral_temp,
				&res.texture_temporal_variance[temporal_output],
			};
			device->BindResources(resarray, 0, arraysize(resarray), cmd);

			const GPUResource* uavs[] = {
				&output,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateRTShadowResources(RTShadowResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R32G32B32A32_UINT;
	device->CreateTexture(&desc, nullptr, &res.temp);
	device->SetName(&res.temp, "rtshadow_temp");
	device->CreateTexture(&desc, nullptr, &res.temporal[0]);
	device->SetName(&res.temporal[0], "rtshadow_temporal[0]");
	device->CreateTexture(&desc, nullptr, &res.temporal[1]);
	device->SetName(&res.temporal[1], "rtshadow_temporal[1]");

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.normals);
	device->SetName(&res.normals, "rtshadow_normals");

	GPUBufferDesc bd;
	bd.stride = sizeof(uint4);
	bd.size = bd.stride *
		((desc.width + 7) / 8) *
		((desc.height + 3) / 4);
	bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateBuffer(&bd, nullptr, &res.tiles);
	device->SetName(&res.tiles, "rtshadow_tiles");
	device->CreateBuffer(&bd, nullptr, &res.metadata);
	device->SetName(&res.metadata, "rtshadow_metadata");

	for (int i = 0; i < 4; ++i)
	{
		desc.format = Format::R16G16_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.scratch[i][0]);
		device->SetName(&res.scratch[i][0], "rtshadow_scratch[i][0]");
		device->CreateTexture(&desc, nullptr, &res.scratch[i][1]);
		device->SetName(&res.scratch[i][1], "rtshadow_scratch[i][1]");

		desc.format = Format::R11G11B10_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.moments[i][0]);
		device->SetName(&res.moments[i][0], "rtshadow_moments[i][0]");
		device->CreateTexture(&desc, nullptr, &res.moments[i][1]);
		device->SetName(&res.moments[i][1], "rtshadow_moments[i][1]");
	}

	desc.format = Format::R8G8B8A8_UNORM;
	device->CreateTexture(&desc, nullptr, &res.denoised);
	device->SetName(&res.denoised, "rtshadow_denoised");
}
void Postprocess_RTShadow(
	const RTShadowResources& res,
	const Scene& scene,
	const GPUBuffer& entityTiles_Opaque,
	const Texture& output,
	CommandList cmd,
	uint8_t instanceInclusionMask
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	device->EventBegin("Postprocess_RTShadow", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("RTShadow", cmd);

	if (res.frame == 0)
	{
		// Maybe we don't need to clear them all, but it's safer this way:
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.temporal[0], res.temporal[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.temporal[1], res.temporal[1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.denoised, res.denoised.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0][0], res.moments[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0][1], res.moments[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1][0], res.moments[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1][1], res.moments[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[2][0], res.moments[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[2][1], res.moments[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[3][0], res.moments[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[3][1], res.moments[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&res.temporal[0], 0, cmd);
		device->ClearUAV(&res.temporal[1], 0, cmd);
		device->ClearUAV(&res.denoised, 0, cmd);
		device->ClearUAV(&res.normals, 0, cmd);
		device->ClearUAV(&res.scratch[0][0], 0, cmd);
		device->ClearUAV(&res.scratch[0][1], 0, cmd);
		device->ClearUAV(&res.scratch[1][0], 0, cmd);
		device->ClearUAV(&res.scratch[1][1], 0, cmd);
		device->ClearUAV(&res.scratch[2][0], 0, cmd);
		device->ClearUAV(&res.scratch[2][1], 0, cmd);
		device->ClearUAV(&res.scratch[3][0], 0, cmd);
		device->ClearUAV(&res.scratch[3][1], 0, cmd);
		device->ClearUAV(&res.moments[0][0], 0, cmd);
		device->ClearUAV(&res.moments[0][1], 0, cmd);
		device->ClearUAV(&res.moments[1][0], 0, cmd);
		device->ClearUAV(&res.moments[1][1], 0, cmd);
		device->ClearUAV(&res.moments[2][0], 0, cmd);
		device->ClearUAV(&res.moments[2][1], 0, cmd);
		device->ClearUAV(&res.moments[3][0], 0, cmd);
		device->ClearUAV(&res.moments[3][1], 0, cmd);
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	const TextureDesc& desc = res.temp.GetDesc();

	BindCommonResources(cmd);

	device->EventBegin("Raytrace", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW], cmd);

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.w = (float)res.frame;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&res.temp,
		&res.normals,
		&res.tiles
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.temp, res.temp.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.tiles, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&res.normals, ResourceState::UNORDERED_ACCESS, res.normals.desc.layout),
			GPUBarrier::Buffer(&res.tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);

	int temporal_output = res.frame % 2;
	int temporal_history = 1 - temporal_output;

	// Denoise - Tile Classification:
	{
		device->EventBegin("Denoise - Tile Classification", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TILECLASSIFICATION], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.tiles, 2, cmd);
		device->BindResource(&res.moments[0][temporal_history], 3, cmd);
		device->BindResource(&res.moments[1][temporal_history], 4, cmd);
		device->BindResource(&res.moments[2][temporal_history], 5, cmd);
		device->BindResource(&res.moments[3][temporal_history], 6, cmd);
		device->BindResource(&res.scratch[0][1], 7, cmd);
		device->BindResource(&res.scratch[1][1], 8, cmd);
		device->BindResource(&res.scratch[2][1], 9, cmd);
		device->BindResource(&res.scratch[3][1], 10, cmd);

		const GPUResource* uavs[] = {
			&res.metadata,
			&res.scratch[0][0],
			&res.scratch[1][0],
			&res.scratch[2][0],
			&res.scratch[3][0],
			&res.moments[0][temporal_output],
			&res.moments[1][temporal_output],
			&res.moments[2][temporal_output],
			&res.moments[3][temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.metadata, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[0][temporal_output], res.moments[0][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[1][temporal_output], res.moments[1][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[2][temporal_output], res.moments[2][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[3][temporal_output], res.moments[3][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			4, // 4 lights
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Buffer(&res.metadata, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Image(&res.scratch[0][0], ResourceState::UNORDERED_ACCESS, res.scratch[0][0].desc.layout),
				GPUBarrier::Image(&res.scratch[1][0], ResourceState::UNORDERED_ACCESS, res.scratch[1][0].desc.layout),
				GPUBarrier::Image(&res.scratch[2][0], ResourceState::UNORDERED_ACCESS, res.scratch[2][0].desc.layout),
				GPUBarrier::Image(&res.scratch[3][0], ResourceState::UNORDERED_ACCESS, res.scratch[3][0].desc.layout),
				GPUBarrier::Image(&res.moments[0][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[0][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[1][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[1][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[2][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[2][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[3][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[3][temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Denoise - Spatial filtering:
	{
		device->EventBegin("Denoise - Filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_FILTER], cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.metadata, 1, cmd);

		// pass0:
		{
			device->BindResource(&res.scratch[0][0], 2, cmd);
			device->BindResource(&res.scratch[1][0], 3, cmd);
			device->BindResource(&res.scratch[2][0], 4, cmd);
			device->BindResource(&res.scratch[3][0], 5, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[0][1],
				&res.scratch[1][1],
				&res.scratch[2][1],
				&res.scratch[3][1],
				&res.denoised
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.denoised, res.denoised.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 0;
			postprocess.params1.y = 1;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				4, // 4 lights
				cmd
			);
		}

		// pass1:
		{
			device->BindResource(&res.scratch[0][1], 2, cmd);
			device->BindResource(&res.scratch[1][1], 3, cmd);
			device->BindResource(&res.scratch[2][1], 4, cmd);
			device->BindResource(&res.scratch[3][1], 5, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[0][0],
				&res.scratch[1][0],
				&res.scratch[2][0],
				&res.scratch[3][0],
				&res.denoised
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.scratch[0][1], ResourceState::UNORDERED_ACCESS, res.scratch[0][1].desc.layout),
					GPUBarrier::Image(&res.scratch[1][1], ResourceState::UNORDERED_ACCESS, res.scratch[1][1].desc.layout),
					GPUBarrier::Image(&res.scratch[2][1], ResourceState::UNORDERED_ACCESS, res.scratch[2][1].desc.layout),
					GPUBarrier::Image(&res.scratch[3][1], ResourceState::UNORDERED_ACCESS, res.scratch[3][1].desc.layout),
					GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 1;
			postprocess.params1.y = 2;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				4, // 4 lights
				cmd
			);
		}

		// pass2:
		{
			device->BindResource(&res.scratch[0][0], 2, cmd);
			device->BindResource(&res.scratch[1][0], 3, cmd);
			device->BindResource(&res.scratch[2][0], 4, cmd);
			device->BindResource(&res.scratch[3][0], 5, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[0][1],
				&res.scratch[1][1],
				&res.scratch[2][1],
				&res.scratch[3][1],
				&res.denoised
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&res.scratch[0][0], ResourceState::UNORDERED_ACCESS, res.scratch[0][0].desc.layout),
					GPUBarrier::Image(&res.scratch[1][0], ResourceState::UNORDERED_ACCESS, res.scratch[1][0].desc.layout),
					GPUBarrier::Image(&res.scratch[2][0], ResourceState::UNORDERED_ACCESS, res.scratch[2][0].desc.layout),
					GPUBarrier::Image(&res.scratch[3][0], ResourceState::UNORDERED_ACCESS, res.scratch[3][0].desc.layout),
					GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 2;
			postprocess.params1.y = 4;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				4, // 4 lights
				cmd
			);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.scratch[0][1], ResourceState::UNORDERED_ACCESS, res.scratch[0][1].desc.layout),
				GPUBarrier::Image(&res.scratch[1][1], ResourceState::UNORDERED_ACCESS, res.scratch[1][1].desc.layout),
				GPUBarrier::Image(&res.scratch[2][1], ResourceState::UNORDERED_ACCESS, res.scratch[2][1].desc.layout),
				GPUBarrier::Image(&res.scratch[3][1], ResourceState::UNORDERED_ACCESS, res.scratch[3][1].desc.layout),
				GPUBarrier::Image(&res.denoised, ResourceState::UNORDERED_ACCESS, res.denoised.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}
	res.frame++;

	// Temporal pass:
	{
		device->EventBegin("Temporal Denoise", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TEMPORAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.temp, 0, cmd);
		device->BindResource(&res.temporal[temporal_history], 1, cmd);
		device->BindResource(&res.denoised, 3, cmd);

		const GPUResource* uavs[] = {
			&res.temporal[temporal_output],
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.temp, ResourceState::UNORDERED_ACCESS, res.temp.desc.layout),
				GPUBarrier::Image(&res.temporal[temporal_output], res.temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateScreenSpaceShadowResources(ScreenSpaceShadowResources& res, XMUINT2 resolution)
{
}
void Postprocess_ScreenSpaceShadow(
	const ScreenSpaceShadowResources& res,
	const GPUBuffer& entityTiles_Opaque,
	const Texture& output,
	CommandList cmd,
	float range,
	uint32_t samplecount
)
{
	device->EventBegin("Postprocess_ScreenSpaceShadow", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("ScreenSpaceShadow", cmd);

	const TextureDesc& desc = output.GetDesc();

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SCREENSPACESHADOW], cmd);

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = range;
	postprocess.params0.y = (float)samplecount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void Postprocess_LightShafts(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	const XMFLOAT2& center,
	float strength
)
{
	device->EventBegin("Postprocess_LightShafts", cmd);
	auto range = wi::profiler::BeginRangeGPU("LightShafts", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_LIGHTSHAFTS], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = 0.65f;		// density
	postprocess.params0.y = 0.25f;		// weight
	postprocess.params0.z = 0.945f;		// decay
	postprocess.params0.w = strength;	// exposure
	postprocess.params1.x = center.x;
	postprocess.params1.y = center.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateDepthOfFieldResources(DepthOfFieldResources& res, XMUINT2 resolution)
{
	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.width = (resolution.x + DEPTHOFFIELD_TILESIZE - 1) / DEPTHOFFIELD_TILESIZE;
	tile_desc.height = (resolution.y + DEPTHOFFIELD_TILESIZE - 1) / DEPTHOFFIELD_TILESIZE;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_neighborhoodmax);
	tile_desc.format = Format::R16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin);

	tile_desc.height = resolution.x;
	tile_desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax_horizontal);
	tile_desc.format = Format::R16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin_horizontal);

	TextureDesc presort_desc;
	presort_desc.type = TextureDesc::Type::TEXTURE_2D;
	presort_desc.width = resolution.x / 2;
	presort_desc.height = resolution.y / 2;
	presort_desc.format = Format::R11G11B10_FLOAT;
	presort_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateTexture(&presort_desc, nullptr, &res.texture_presort);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_prefilter);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_main);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_postfilter);
	presort_desc.format = Format::R8_UNORM;
	device->CreateTexture(&presort_desc, nullptr, &res.texture_alpha1);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_alpha2);


	GPUBufferDesc bufferdesc;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	bufferdesc.size = TILE_STATISTICS_CAPACITY * sizeof(uint);
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_RAW | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_statistics);

	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_desc.width * tile_desc.height * bufferdesc.stride;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_expensive);
}
void Postprocess_DepthOfField(
	const DepthOfFieldResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float coc_scale,
	float max_coc
)
{
	device->EventBegin("Postprocess_DepthOfField", cmd);
	auto range = wi::profiler::BeginRangeGPU("Depth of Field", cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	dof_cocscale = coc_scale;
	dof_maxcoc = max_coc;

	// Compute tile max COC (horizontal):
	{
		device->EventBegin("TileMax - Horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_HORIZONTAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax_horizontal, res.texture_tilemax_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, res.texture_tilemin_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tilemax_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tilemax_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemax_horizontal.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemin_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile max COC (vertical):
	{
		device->EventBegin("TileMax - Vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_VERTICAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax,
			&res.texture_tilemin,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax, res.texture_tilemax.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_tilemin, res.texture_tilemin.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tilemax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tilemax, ResourceState::UNORDERED_ACCESS, res.texture_tilemax.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin, ResourceState::UNORDERED_ACCESS, res.texture_tilemin.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute max COC for each tiles' neighborhood
	{
		device->EventBegin("NeighborhoodMax", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_NEIGHBORHOODMAXCOC], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax,
			&res.texture_tilemin
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
			&res.texture_neighborhoodmax
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_neighborhoodmax, res.texture_neighborhoodmax.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_neighborhoodmax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_neighborhoodmax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_neighborhoodmax, ResourceState::UNORDERED_ACCESS, res.texture_neighborhoodmax.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Kick indirect tile jobs:
	{
		device->EventBegin("Kickjobs", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_KICKJOBS], cmd);

		device->BindResource(&res.texture_tilemax, 0, cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(1, 1, 1, cmd);

		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->EventEnd(cmd);
	}

	// Switch to half res:
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Prepass:
	{
		device->EventBegin("Prepass", cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_neighborhoodmax,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_presort,
			&res.texture_prefilter
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.texture_presort, res.texture_presort.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_prefilter, res.texture_prefilter.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindResource(&res.buffer_tiles_earlyexit, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS_EARLYEXIT], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EARLYEXIT, cmd);

		device->BindResource(&res.buffer_tiles_cheap, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_CHEAP, cmd);

		device->BindResource(&res.buffer_tiles_expensive, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EXPENSIVE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_presort, ResourceState::UNORDERED_ACCESS, res.texture_presort.desc.layout),
				GPUBarrier::Image(&res.texture_prefilter, ResourceState::UNORDERED_ACCESS, res.texture_prefilter.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Main pass:
	{
		device->EventBegin("Main pass", cmd);

		const GPUResource* resarray[] = {
			&res.texture_neighborhoodmax,
			&res.texture_presort,
			&res.texture_prefilter,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_main,
			&res.texture_alpha1
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_main, res.texture_main.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_alpha1, res.texture_alpha1.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_EARLYEXIT], cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EARLYEXIT, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_CHEAP], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_CHEAP, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EXPENSIVE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_main, ResourceState::UNORDERED_ACCESS, res.texture_main.desc.layout),
				GPUBarrier::Image(&res.texture_alpha1, ResourceState::UNORDERED_ACCESS, res.texture_alpha1.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Post filter:
	{
		device->EventBegin("Post filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_POSTFILTER], cmd);

		const GPUResource* resarray[] = {
			&res.texture_main,
			&res.texture_alpha1
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_postfilter,
			&res.texture_alpha2
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_postfilter, res.texture_postfilter.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_alpha2, res.texture_alpha2.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_postfilter.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_postfilter.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_postfilter, ResourceState::UNORDERED_ACCESS, res.texture_postfilter.desc.layout),
				GPUBarrier::Image(&res.texture_alpha2, ResourceState::UNORDERED_ACCESS, res.texture_alpha2.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Switch to full res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Upsample pass:
	{
		device->EventBegin("Upsample pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_postfilter,
			&res.texture_alpha2,
			&res.texture_neighborhoodmax
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Outline(
	const Texture& input,
	CommandList cmd,
	float threshold,
	float thickness,
	const XMFLOAT4& color
)
{
	device->EventBegin("Postprocess_Outline", cmd);
	auto range = wi::profiler::BeginRangeGPU("Outline", cmd);

	device->BindPipelineState(&PSO_outline, cmd);

	device->BindResource(&input, 0, cmd);

	PostProcess postprocess;
	postprocess.resolution.x = (uint)input.GetDesc().width;
	postprocess.resolution.y = (uint)input.GetDesc().height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = threshold;
	postprocess.params0.y = thickness;
	postprocess.params1.x = color.x;
	postprocess.params1.y = color.y;
	postprocess.params1.z = color.z;
	postprocess.params1.w = color.w;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->Draw(3, 0, cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateMotionBlurResources(MotionBlurResources& res, XMUINT2 resolution)
{
	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.width = (resolution.x + MOTIONBLUR_TILESIZE - 1) / MOTIONBLUR_TILESIZE;
	tile_desc.height = (resolution.y + MOTIONBLUR_TILESIZE - 1) / MOTIONBLUR_TILESIZE;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_neighborhoodmax);

	tile_desc.height = resolution.y;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax_horizontal);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin_horizontal);


	GPUBufferDesc bufferdesc;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	bufferdesc.size = TILE_STATISTICS_CAPACITY * sizeof(uint);
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_RAW | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_statistics);

	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_desc.width * tile_desc.height * bufferdesc.stride;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_expensive);
}
void Postprocess_MotionBlur(
	const MotionBlurResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float strength
)
{
	device->EventBegin("Postprocess_MotionBlur", cmd);
	auto range = wi::profiler::BeginRangeGPU("MotionBlur", cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	motionblur_strength = strength / 60.0f; // align to shutter speed

	// Compute tile max velocities (horizontal):
	{
		device->EventBegin("TileMax - Horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_HORIZONTAL], cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax_horizontal, res.texture_tilemax_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, res.texture_tilemin_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tilemax_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tilemax_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemax_horizontal.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemin_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile max velocities (vertical):
	{
		device->EventBegin("TileMax - Vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_VERTICAL], cmd);

		device->BindResource(&res.texture_tilemax_horizontal, 0, cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax,
			&res.texture_tilemin,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax, res.texture_tilemax.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_tilemin, res.texture_tilemin.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_tilemax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_tilemax, ResourceState::UNORDERED_ACCESS, res.texture_tilemax.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin, ResourceState::UNORDERED_ACCESS, res.texture_tilemin.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute max velocities for each tiles' neighborhood
	{
		device->EventBegin("NeighborhoodMax", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_NEIGHBORHOODMAXVELOCITY], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax,
			&res.texture_tilemin
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
			&res.texture_neighborhoodmax
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_neighborhoodmax, res.texture_neighborhoodmax.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_neighborhoodmax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_neighborhoodmax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_neighborhoodmax, ResourceState::UNORDERED_ACCESS, res.texture_neighborhoodmax.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Kick indirect tile jobs:
	{
		device->EventBegin("Kickjobs", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_KICKJOBS], cmd);

		device->BindResource(&res.texture_tilemax, 0, cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(1, 1, 1, cmd);

		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->EventEnd(cmd);
	}

	// Tile jobs:
	{
		device->EventBegin("MotionBlur Jobs", cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_neighborhoodmax,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_EARLYEXIT], cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EARLYEXIT, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_CHEAP], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_CHEAP, cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, INDIRECT_OFFSET_EXPENSIVE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateVolumetricCloudResources(VolumetricCloudResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	XMUINT2 renderResolution = XMUINT2(resolution.x / 4, resolution.y / 4);
	XMUINT2 reprojectionResolution = XMUINT2(resolution.x / 2, resolution.y / 2);
	XMUINT2 maskResolution = XMUINT2(resolution.x / 4, resolution.y / 4); // Needs to be half of final cloud output

	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.width = renderResolution.x;
	desc.height = renderResolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.texture_cloudRender);
	device->SetName(&res.texture_cloudRender, "texture_cloudRender");
	desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_cloudDepth);
	device->SetName(&res.texture_cloudDepth, "texture_cloudDepth");

	desc.width = reprojectionResolution.x;
	desc.height = reprojectionResolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_reproject[0]);
	device->SetName(&res.texture_reproject[0], "texture_reproject[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_reproject[1]);
	device->SetName(&res.texture_reproject[1], "texture_reproject[1]");
	desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_depth[0]);
	device->SetName(&res.texture_reproject_depth[0], "texture_reproject_depth[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_depth[1]);
	device->SetName(&res.texture_reproject_depth[1], "texture_reproject_depth[1]");

	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->SetName(&res.texture_temporal[0], "texture_temporal[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	device->SetName(&res.texture_temporal[1], "texture_temporal[1]");

	desc.width = maskResolution.x;
	desc.height = maskResolution.y;
	desc.format = Format::R8G8B8A8_UNORM;
	device->CreateTexture(&desc, nullptr, &res.texture_cloudMask);
	device->SetName(&res.texture_cloudMask, "texture_cloudMask");
}
void Postprocess_VolumetricClouds(
	const VolumetricCloudResources& res,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_VolumetricClouds", cmd);
	auto range = wi::profiler::BeginRangeGPU("Volumetric Clouds", cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = res.texture_reproject[0].GetDesc();
	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = (float)res.texture_reproject[0].GetDesc().width;
	postprocess.params0.y = (float)res.texture_reproject[0].GetDesc().height;
	postprocess.params0.z = 1.0f / postprocess.params0.x;
	postprocess.params0.w = 1.0f / postprocess.params0.y;

	// Cloud pass:
	{
		device->EventBegin("Volumetric Cloud Rendering", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&texture_shapeNoise, 1, cmd);
		device->BindResource(&texture_detailNoise, 2, cmd);
		device->BindResource(&texture_curlNoise, 3, cmd);
		device->BindResource(&texture_weatherMap, 4, cmd);

		const GPUResource* uavs[] = {
			&res.texture_cloudRender,
			&res.texture_cloudDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_cloudRender, res.texture_cloudRender.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_cloudDepth, res.texture_cloudDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_cloudRender.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_cloudRender.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_cloudRender, ResourceState::UNORDERED_ACCESS, res.texture_cloudRender.desc.layout),
				GPUBarrier::Image(&res.texture_cloudDepth, ResourceState::UNORDERED_ACCESS, res.texture_cloudDepth.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	const TextureDesc& reprojection_desc = res.texture_reproject[0].GetDesc();
	postprocess.resolution.x = reprojection_desc.width;
	postprocess.resolution.y = reprojection_desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	volumetricclouds_frame = (float)res.frame;
	
	int temporal_output = device->GetFrameCount() % 2;
	int temporal_history = 1 - temporal_output;

	// Reprojection pass:
	{
		device->EventBegin("Volumetric Cloud Reproject", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_REPROJECT], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.texture_cloudRender, 0, cmd);
		device->BindResource(&res.texture_cloudDepth, 1, cmd);
		device->BindResource(&res.texture_reproject[temporal_history], 2, cmd);
		device->BindResource(&res.texture_reproject_depth[temporal_history], 3, cmd);

		const GPUResource* uavs[] = {
			&res.texture_reproject[temporal_output],
			&res.texture_reproject_depth[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_reproject[temporal_output], res.texture_reproject[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_reproject_depth[temporal_output], res.texture_reproject_depth[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_reproject[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_reproject[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_reproject[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_reproject[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_reproject_depth[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_reproject_depth[temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Temporal pass:
	{
		device->EventBegin("Volumetric Cloud Temporal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_TEMPORAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.texture_reproject[temporal_output], 0, cmd);
		device->BindResource(&res.texture_reproject_depth[temporal_output], 1, cmd);
		device->BindResource(&res.texture_temporal[temporal_history], 2, cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_cloudMask,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_cloudMask, res.texture_cloudMask.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_cloudMask, ResourceState::UNORDERED_ACCESS, res.texture_cloudMask.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_FXAA(
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_FXAA", cmd);
	auto range = wi::profiler::BeginRangeGPU("FXAA", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FXAA], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateTemporalAAResources(TemporalAAResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.format = Format::R11G11B10_FLOAT;
	desc.width = resolution.x;
	desc.height = resolution.y;
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->SetName(&res.texture_temporal[0], "TemporalAAResources::texture_temporal[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	device->SetName(&res.texture_temporal[1], "TemporalAAResources::texture_temporal[1]");
}
void Postprocess_TemporalAA(
	const TemporalAAResources& res,
	const Texture& input,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_TemporalAA", cmd);
	auto range = wi::profiler::BeginRangeGPU("Temporal AA Resolve", cmd);
	const bool first_frame = res.frame == 0;
	res.frame++;

	if (first_frame)
	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_temporal[0], res.texture_temporal[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[1], res.texture_temporal[1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->ClearUAV(&res.texture_temporal[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal[1], 0, cmd);

		std::swap(barriers[0].image.layout_before, barriers[0].image.layout_after);
		std::swap(barriers[1].image.layout_before, barriers[1].image.layout_after);
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_TEMPORALAA], cmd);

	device->BindResource(&input, 0, cmd);
	if (first_frame)
	{
		device->BindResource(&input, 0, cmd);
	}
	else
	{
		device->BindResource(res.GetHistory(), 1, cmd);
	}

	const TextureDesc& desc = res.texture_temporal[0].GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const Texture* output = res.GetCurrent();

	const GPUResource* uavs[] = {
		output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(output, output->GetDesc().layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(output, ResourceState::UNORDERED_ACCESS, output->GetDesc().layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Sharpen(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float amount
)
{
	device->EventBegin("Postprocess_Sharpen", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SHARPEN], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = amount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	device->EventEnd(cmd);
}
void Postprocess_Tonemap(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float exposure,
	bool dither,
	const Texture* texture_colorgradinglut,
	const Texture* texture_distortion,
	const GPUBuffer* buffer_luminance,
	const Texture* texture_bloom,
	ColorSpace display_colorspace
)
{
	if (!input.IsValid() || !output.IsValid())
	{
		assert(0);
		return;
	}

	device->EventBegin("Postprocess_Tonemap", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_TONEMAP], cmd);

	const TextureDesc& desc = output.GetDesc();

	assert(texture_colorgradinglut == nullptr || texture_colorgradinglut->desc.type == TextureDesc::Type::TEXTURE_3D); // This must be a 3D lut

	PushConstantsTonemap tonemap_push = {};
	tonemap_push.resolution_rcp.x = 1.0f / desc.width;
	tonemap_push.resolution_rcp.y = 1.0f / desc.height;
	tonemap_push.exposure = exposure;
	tonemap_push.dither = dither ? 1.0f : 0.0f;
	tonemap_push.texture_input = device->GetDescriptorIndex(&input, SubresourceType::SRV);
	tonemap_push.buffer_input_luminance = device->GetDescriptorIndex((buffer_luminance == nullptr) ? &luminance_dummy : buffer_luminance, SubresourceType::SRV);
	tonemap_push.texture_input_distortion = device->GetDescriptorIndex(texture_distortion, SubresourceType::SRV);
	tonemap_push.texture_colorgrade_lookuptable = device->GetDescriptorIndex(texture_colorgradinglut, SubresourceType::SRV);
	tonemap_push.texture_bloom = device->GetDescriptorIndex(texture_bloom, SubresourceType::SRV);
	tonemap_push.texture_output = device->GetDescriptorIndex(&output, SubresourceType::UAV);
	tonemap_push.display_colorspace = static_cast<uint32_t>(display_colorspace);
	device->PushConstants(&tonemap_push, sizeof(tonemap_push), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	device->EventEnd(cmd);
}

#define A_CPU
#include "shaders/ffx-fsr/ffx_a.h"
#include "shaders/ffx-fsr/ffx_fsr1.h"
void Postprocess_FSR(
	const Texture& input,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	float sharpness
)
{
	device->EventBegin("Postprocess_FSR", cmd);
	auto range = wi::profiler::BeginRangeGPU("Postprocess_FSR", cmd);

	const TextureDesc& desc = output.GetDesc();

	struct FSR
	{
		AU1 const0[4];
		AU1 const1[4];
		AU1 const2[4];
		AU1 const3[4];
	} fsr;

	// Upscaling:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR_UPSCALING], cmd);

		FsrEasuCon(
			fsr.const0,
			fsr.const1,
			fsr.const2,
			fsr.const3,

			// current frame render resolution:
			static_cast<AF1>(input.desc.width),
			static_cast<AF1>(input.desc.height),

			// input container resolution:
			static_cast<AF1>(input.desc.width),
			static_cast<AF1>(input.desc.height),

			// upscaled-to-resolution:
			static_cast<AF1>(temp.desc.width),
			static_cast<AF1>(temp.desc.height)

		);
		device->BindDynamicConstantBuffer(fsr, CBSLOT_FSR, cmd);

		device->BindResource(&input, 0, cmd);

		const GPUResource* uavs[] = {
			&temp,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Sharpen:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR_SHARPEN], cmd);

		FsrRcasCon(fsr.const0, sharpness);
		device->BindDynamicConstantBuffer(fsr, CBSLOT_FSR, cmd);

		device->BindResource(&temp, 0, cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Chromatic_Aberration(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float amount
)
{
	device->EventBegin("Postprocess_Chromatic_Aberration", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_CHROMATIC_ABERRATION], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = amount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	device->EventEnd(cmd);
}
void Postprocess_Upsample_Bilateral(
	const Texture& input,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	bool pixelshader,
	float threshold
)
{
	device->EventBegin("Postprocess_Upsample_Bilateral", cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = threshold;
	postprocess.params0.y = 1.0f / (float)input.GetDesc().width;
	postprocess.params0.z = 1.0f / (float)input.GetDesc().height;
	// select mip from lowres depth mipchain:
	postprocess.params0.w = std::floor(std::max(1.0f, log2f(std::max((float)desc.width / (float)input.GetDesc().width, (float)desc.height / (float)input.GetDesc().height))));
	postprocess.params1.x = (float)input.GetDesc().width;
	postprocess.params1.y = (float)input.GetDesc().height;
	postprocess.params1.z = 1.0f / postprocess.params1.x;
	postprocess.params1.w = 1.0f / postprocess.params1.y;

	if (pixelshader)
	{
		device->BindPipelineState(&PSO_upsample_bilateral, cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd);

		device->Draw(3, 0, cmd);
	}
	else
	{
		SHADERTYPE cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4;
		switch (desc.format)
		{
		case Format::R16_UNORM:
		case Format::R8_UNORM:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UNORM1;
			break;
		case Format::R16_FLOAT:
		case Format::R32_FLOAT:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT1;
			break;
		case Format::R16G16B16A16_UNORM:
		case Format::R8G8B8A8_UNORM:
		case Format::B8G8R8A8_UNORM:
		case Format::R10G10B10A2_UNORM:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UNORM4;
			break;
		case Format::R11G11B10_FLOAT:
		case Format::R16G16B16A16_FLOAT:
		case Format::R32G32B32A32_FLOAT:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4;
			break;
		case Format::R32G32B32A32_UINT:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_UINT4;
			break;
		default:
			assert(0); // implement format!
			break;
		}
		device->BindComputeShader(&shaders[cs], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	device->EventEnd(cmd);
}
void Postprocess_Downsample4x(
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_Downsample4x", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DOWNSAMPLE4X], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}
void Postprocess_NormalsFromDepth(
	const Texture& depthbuffer,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_NormalsFromDepth", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_NORMALSFROMDEPTH], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = std::floor(std::max(1.0f, log2f(std::max((float)desc.width / (float)depthbuffer.GetDesc().width, (float)desc.height / (float)depthbuffer.GetDesc().height))));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&depthbuffer, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}
void Postprocess_Underwater(
	const wi::graphics::Texture& input,
	const wi::graphics::Texture& output,
	wi::graphics::CommandList cmd
)
{
	device->EventBegin("Postprocess_Underwater", cmd);
	auto range = wi::profiler::BeginRangeGPU("Underwater", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_UNDERWATER], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}


Ray GetPickRay(long cursorX, long cursorY, const wi::Canvas& canvas, const CameraComponent& camera)
{
	float screenW = canvas.GetLogicalWidth();
	float screenH = canvas.GetLogicalHeight();

	XMMATRIX V = camera.GetView();
	XMMATRIX P = camera.GetProjection();
	XMMATRIX W = XMMatrixIdentity();
	XMVECTOR lineStart = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 1, 1), 0, 0, screenW, screenH, 0.0f, 1.0f, P, V, W);
	XMVECTOR lineEnd = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 0, 1), 0, 0, screenW, screenH, 0.0f, 1.0f, P, V, W);
	XMVECTOR rayDirection = XMVector3Normalize(XMVectorSubtract(lineEnd, lineStart));
	return Ray(lineStart, rayDirection);
}

void DrawBox(const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color)
{
	renderableBoxes.push_back(std::make_pair(boxMatrix,color));
}
void DrawSphere(const Sphere& sphere, const XMFLOAT4& color)
{
	renderableSpheres.push_back(std::make_pair(sphere, color));
}
void DrawCapsule(const Capsule& capsule, const XMFLOAT4& color)
{
	renderableCapsules.push_back(std::make_pair(capsule, color));
}
void DrawLine(const RenderableLine& line)
{
	renderableLines.push_back(line);
}
void DrawLine(const RenderableLine2D& line)
{
	renderableLines2D.push_back(line);
}
void DrawPoint(const RenderablePoint& point)
{
	renderablePoints.push_back(point);
}
void DrawTriangle(const RenderableTriangle& triangle, bool wireframe)
{
	if (wireframe)
	{
		renderableTriangles_wireframe.push_back(triangle);
	}
	else
	{
		renderableTriangles_solid.push_back(triangle);
	}
}
void DrawDebugText(const char* text, const DebugTextParams& params)
{
	for (size_t i = 0; i < sizeof(DebugTextParams); ++i)
	{
		debugTextStorage.push_back(((uint8_t*)(&params))[i]);
	}
	size_t len = strlen(text) + 1;
	for (size_t i = 0; i < len; ++i)
	{
		debugTextStorage.push_back(uint8_t(text[i]));
	}
}
void DrawPaintRadius(const PaintRadius& paintrad)
{
	paintrads.push_back(paintrad);
}

void AddDeferredMIPGen(const Texture& texture, bool preserve_coverage)
{
	deferredMIPGenLock.lock();
	deferredMIPGens.push_back(std::make_pair(texture, preserve_coverage));
	deferredMIPGenLock.unlock();
}



void SetTransparentShadowsEnabled(float value) { TRANSPARENTSHADOWSENABLED = value; }
float GetTransparentShadowsEnabled() { return TRANSPARENTSHADOWSENABLED; }
void SetWireRender(bool value) { wireRender = value; }
bool IsWireRender() { return wireRender; }
void SetToDrawDebugBoneLines(bool param) { debugBoneLines = param; }
bool GetToDrawDebugBoneLines() { return debugBoneLines; }
void SetToDrawDebugPartitionTree(bool param) { debugPartitionTree = param; }
bool GetToDrawDebugPartitionTree() { return debugPartitionTree; }
bool GetToDrawDebugEnvProbes() { return debugEnvProbes; }
void SetToDrawDebugEnvProbes(bool value) { debugEnvProbes = value; }
void SetToDrawDebugEmitters(bool param) { debugEmitters = param; }
bool GetToDrawDebugEmitters() { return debugEmitters; }
void SetToDrawDebugForceFields(bool param) { debugForceFields = param; }
bool GetToDrawDebugForceFields() { return debugForceFields; }
void SetToDrawDebugCameras(bool param) { debugCameras = param; }
bool GetToDrawDebugCameras() { return debugCameras; }
bool GetToDrawGridHelper() { return gridHelper; }
void SetToDrawGridHelper(bool value) { gridHelper = value; }
bool GetToDrawVoxelHelper() { return voxelHelper; }
void SetToDrawVoxelHelper(bool value) { voxelHelper = value; }
void SetDebugLightCulling(bool enabled) { debugLightCulling = enabled; }
bool GetDebugLightCulling() { return debugLightCulling; }
void SetAdvancedLightCulling(bool enabled) { advancedLightCulling = enabled; }
bool GetAdvancedLightCulling() { return advancedLightCulling; }
void SetVariableRateShadingClassification(bool enabled) { variableRateShadingClassification = enabled; }
bool GetVariableRateShadingClassification() { return variableRateShadingClassification; }
void SetVariableRateShadingClassificationDebug(bool enabled) { variableRateShadingClassificationDebug = enabled; }
bool GetVariableRateShadingClassificationDebug() { return variableRateShadingClassificationDebug; }
void SetOcclusionCullingEnabled(bool value)
{
	occlusionCulling = value;
}
bool GetOcclusionCullingEnabled() { return occlusionCulling; }
void SetTemporalAAEnabled(bool enabled) { temporalAA = enabled; }
bool GetTemporalAAEnabled() { return temporalAA; }
void SetTemporalAADebugEnabled(bool enabled) { temporalAADEBUG = enabled; }
bool GetTemporalAADebugEnabled() { return temporalAADEBUG; }
void SetFreezeCullingCameraEnabled(bool enabled) { freezeCullingCamera = enabled; }
bool GetFreezeCullingCameraEnabled() { return freezeCullingCamera; }
void SetVoxelRadianceEnabled(bool enabled)
{
	voxelSceneData.enabled = enabled;
	if (!textures[TEXTYPE_3D_VOXELRADIANCE].IsValid())
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_3D;
		desc.width = voxelSceneData.res;
		desc.height = voxelSceneData.res;
		desc.depth = voxelSceneData.res;
		desc.mip_levels = 0;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
		desc.usage = Usage::DEFAULT;

		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_3D_VOXELRADIANCE]);

		for (uint32_t i = 0; i < textures[TEXTYPE_3D_VOXELRADIANCE].GetDesc().mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&textures[TEXTYPE_3D_VOXELRADIANCE], SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&textures[TEXTYPE_3D_VOXELRADIANCE], SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}
	}
	if (!textures[TEXTYPE_3D_VOXELRADIANCE_HELPER].IsValid())
	{
		const TextureDesc& desc = textures[TEXTYPE_3D_VOXELRADIANCE].GetDesc();
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_3D_VOXELRADIANCE_HELPER]);

		for (uint32_t i = 0; i < desc.mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&textures[TEXTYPE_3D_VOXELRADIANCE_HELPER], SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&textures[TEXTYPE_3D_VOXELRADIANCE_HELPER], SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}
	}
	if (!resourceBuffers[RBTYPE_VOXELSCENE].IsValid())
	{
		GPUBufferDesc desc;
		desc.stride = sizeof(uint32_t) * 2;
		desc.size = desc.stride * voxelSceneData.res * voxelSceneData.res * voxelSceneData.res;
		desc.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
		desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
		desc.usage = Usage::DEFAULT;

		device->CreateBuffer(&desc, nullptr, &resourceBuffers[RBTYPE_VOXELSCENE]);
	}
}
bool GetVoxelRadianceEnabled() { return voxelSceneData.enabled; }
void SetVoxelRadianceSecondaryBounceEnabled(bool enabled) { voxelSceneData.secondaryBounceEnabled = enabled; }
bool GetVoxelRadianceSecondaryBounceEnabled() { return voxelSceneData.secondaryBounceEnabled; }
void SetVoxelRadianceReflectionsEnabled(bool enabled) { voxelSceneData.reflectionsEnabled = enabled; }
bool GetVoxelRadianceReflectionsEnabled() { return voxelSceneData.reflectionsEnabled; }
void SetVoxelRadianceVoxelSize(float value) { voxelSceneData.voxelsize = value; }
float GetVoxelRadianceVoxelSize() { return voxelSceneData.voxelsize; }
void SetVoxelRadianceMaxDistance(float value) { voxelSceneData.maxDistance = value; }
float GetVoxelRadianceMaxDistance() { return voxelSceneData.maxDistance; }
int GetVoxelRadianceResolution() { return voxelSceneData.res; }
void SetVoxelRadianceNumCones(int value) { voxelSceneData.numCones = value; }
int GetVoxelRadianceNumCones() { return voxelSceneData.numCones; }
float GetVoxelRadianceRayStepSize() { return voxelSceneData.rayStepSize; }
void SetVoxelRadianceRayStepSize(float value) { voxelSceneData.rayStepSize = value; }
void SetGameSpeed(float value) { GameSpeed = std::max(0.0f, value); }
float GetGameSpeed() { return GameSpeed; }
void SetRaytraceBounceCount(uint32_t bounces)
{
	raytraceBounceCount = bounces;
}
uint32_t GetRaytraceBounceCount()
{
	return raytraceBounceCount;
}
void SetRaytraceDebugBVHVisualizerEnabled(bool value)
{
	raytraceDebugVisualizer = value;
}
bool GetRaytraceDebugBVHVisualizerEnabled()
{
	return raytraceDebugVisualizer;
}
void SetRaytracedShadowsEnabled(bool value)
{
	raytracedShadows = value;
}
bool GetRaytracedShadowsEnabled()
{
	return raytracedShadows;
}
void SetTessellationEnabled(bool value)
{
	tessellationEnabled = value;
}
bool GetTessellationEnabled()
{
	return tessellationEnabled;
}
void SetDisableAlbedoMaps(bool value)
{
	disableAlbedoMaps = value;
}
bool IsDisableAlbedoMaps()
{
	return disableAlbedoMaps;
}
void SetForceDiffuseLighting(bool value)
{
	forceDiffuseLighting = value;
}
bool IsForceDiffuseLighting()
{
	return forceDiffuseLighting;
}
void SetScreenSpaceShadowsEnabled(bool value)
{
	SCREENSPACESHADOWS = value;
}
bool GetScreenSpaceShadowsEnabled()
{
	return SCREENSPACESHADOWS;
}
void SetSurfelGIEnabled(bool value)
{
	SURFELGI = value;
}
bool GetSurfelGIEnabled()
{
	return SURFELGI;
}
void SetSurfelGIDebugEnabled(SURFEL_DEBUG value)
{
	SURFELGI_DEBUG = value;
}
SURFEL_DEBUG GetSurfelGIDebugEnabled()
{
	return SURFELGI_DEBUG;
}
void SetDDGIEnabled(bool value)
{
	DDGI_ENABLED = value;
}
bool GetDDGIEnabled()
{
	return DDGI_ENABLED;
}
void SetDDGIDebugEnabled(bool value)
{
	DDGI_DEBUG_ENABLED = value;
}
bool GetDDGIDebugEnabled()
{
	return DDGI_DEBUG_ENABLED;
}
void SetDDGIRayCount(uint32_t value)
{
	DDGI_RAYCOUNT = value;
}
uint32_t GetDDGIRayCount()
{
	return DDGI_RAYCOUNT;
}
void SetGIBoost(float value)
{
	GI_BOOST = value;
}
float GetGIBoost()
{
	return GI_BOOST;
}

}
