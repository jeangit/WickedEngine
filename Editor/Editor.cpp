#include "stdafx.h"
#include "Editor.h"
#include "wiRenderer.h"
#include "wiScene_BindLua.h"

#include "ModelImporter.h"
#include "Translator.h"

#include "FontAwesomeV6.h" // font TTF data

#include <string>
#include <cassert>
#include <cmath>
#include <limits>

using namespace wi::graphics;
using namespace wi::primitive;
using namespace wi::scene;
using namespace wi::ecs;

void Editor::Initialize()
{
	Application::Initialize();

	// With this mode, file data for resources will be kept around. This allows serializing embedded resource data inside scenes
	wi::resourcemanager::SetMode(wi::resourcemanager::Mode::ALLOW_RETAIN_FILEDATA);

	infoDisplay.active = true;
	infoDisplay.watermark = false; // can be toggled instead on gui
	//infoDisplay.fpsinfo = true;
	//infoDisplay.resolution = true;
	//infoDisplay.logical_size = true;
	//infoDisplay.colorspace = true;
	//infoDisplay.heap_allocation_counter = true;
	//infoDisplay.vram_usage = true;

	wi::renderer::SetOcclusionCullingEnabled(true);

	loader.Load();

	renderComponent.main = this;

	loader.addLoadingComponent(&renderComponent, this, 0.2f);

	ActivatePath(&loader, 0.2f);

}

void EditorLoadingScreen::Load()
{
	font = wi::SpriteFont("Loading...", wi::font::Params(0, 0, 36, wi::font::WIFALIGN_LEFT, wi::font::WIFALIGN_CENTER));
	font.anim.typewriter.time = 2;
	font.anim.typewriter.looped = true;
	font.anim.typewriter.character_start = 7;
	AddFont(&font);

	sprite.anim.opa = 1;
	sprite.anim.repeatable = true;
	sprite.params.siz = XMFLOAT2(128, 128);
	sprite.params.pivot = XMFLOAT2(0.5f, 1.0f);
	sprite.params.quality = wi::image::QUALITY_LINEAR;
	sprite.params.blendFlag = wi::enums::BLENDMODE_ALPHA;
	AddSprite(&sprite);

	wi::gui::CheckBox::SetCheckTextGlobal(ICON_CHECK);

	LoadingScreen::Load();
}
void EditorLoadingScreen::Update(float dt)
{
	font.params.posX = GetLogicalWidth()*0.5f - font.TextWidth() * 0.5f;
	font.params.posY = GetLogicalHeight()*0.5f;
	sprite.params.pos = XMFLOAT3(GetLogicalWidth()*0.5f, GetLogicalHeight()*0.5f - font.TextHeight(), 0);
	sprite.textureResource.SetTexture(*wi::texturehelper::getLogo()); // use embedded asset

	LoadingScreen::Update(dt);
}

void EditorComponent::ChangeRenderPath(RENDERPATH path)
{
	switch (path)
	{
	case EditorComponent::RENDERPATH_DEFAULT:
		renderPath = std::make_unique<wi::RenderPath3D>();
		optionsWnd.pathTraceTargetSlider.SetVisible(false);
		optionsWnd.pathTraceStatisticsLabel.SetVisible(false);
		break;
	case EditorComponent::RENDERPATH_PATHTRACING:
		renderPath = std::make_unique<wi::RenderPath3D_PathTracing>();
		optionsWnd.pathTraceTargetSlider.SetVisible(true);
		optionsWnd.pathTraceStatisticsLabel.SetVisible(true);
		break;
	default:
		assert(0);
		break;
	}

	if (scenes.empty())
	{
		NewScene();
	}
	else
	{
		SetCurrentScene(current_scene);
	}

	renderPath->resolutionScale = resolutionScale;
	renderPath->setBloomThreshold(3.0f);

	renderPath->Load();

	// Destroy and recreate renderer and postprocess windows:

	optionsWnd.RemoveWidget(&optionsWnd.rendererWnd);
	optionsWnd.rendererWnd = {};
	optionsWnd.rendererWnd.Create(this);
	optionsWnd.AddWidget(&optionsWnd.rendererWnd);

	optionsWnd.RemoveWidget(&optionsWnd.postprocessWnd);
	optionsWnd.postprocessWnd = {};
	optionsWnd.postprocessWnd.Create(this);
	optionsWnd.AddWidget(&optionsWnd.postprocessWnd);

	optionsWnd.themeCombo.SetSelected(optionsWnd.themeCombo.GetSelected()); // destroyed windows need theme set again
}

void EditorComponent::ResizeBuffers()
{
	optionsWnd.rendererWnd.UpdateSwapChainFormats(&main->swapChain);

	init(main->canvas);
	RenderPath2D::ResizeBuffers();

	GraphicsDevice* device = wi::graphics::GetDevice();

	renderPath->init(*this);
	renderPath->ResizeBuffers();

	if(renderPath->GetDepthStencil() != nullptr)
	{
		bool success = false;

		XMUINT2 internalResolution = GetInternalResolution();

		TextureDesc desc;
		desc.width = internalResolution.x;
		desc.height = internalResolution.y;

		desc.format = Format::R8_UNORM;
		desc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE;
		if (renderPath->getMSAASampleCount() > 1)
		{
			desc.sample_count = renderPath->getMSAASampleCount();
			success = device->CreateTexture(&desc, nullptr, &rt_selectionOutline_MSAA);
			assert(success);
			desc.sample_count = 1;
		}
		success = device->CreateTexture(&desc, nullptr, &rt_selectionOutline[0]);
		assert(success);
		success = device->CreateTexture(&desc, nullptr, &rt_selectionOutline[1]);
		assert(success);

		{
			RenderPassDesc desc;
			desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rt_selectionOutline[0], RenderPassAttachment::LoadOp::CLEAR));
			if (renderPath->getMSAASampleCount() > 1)
			{
				desc.attachments[0].texture = &rt_selectionOutline_MSAA;
				desc.attachments.push_back(RenderPassAttachment::Resolve(&rt_selectionOutline[0]));
			}
			desc.attachments.push_back(
				RenderPassAttachment::DepthStencil(
					renderPath->GetDepthStencil(),
					RenderPassAttachment::LoadOp::LOAD,
					RenderPassAttachment::StoreOp::STORE,
					ResourceState::DEPTHSTENCIL_READONLY,
					ResourceState::DEPTHSTENCIL_READONLY,
					ResourceState::DEPTHSTENCIL_READONLY
				)
			);
			success = device->CreateRenderPass(&desc, &renderpass_selectionOutline[0]);
			assert(success);

			if (renderPath->getMSAASampleCount() == 1)
			{
				desc.attachments[0].texture = &rt_selectionOutline[1]; // rendertarget
			}
			else
			{
				desc.attachments[1].texture = &rt_selectionOutline[1]; // resolve
			}
			success = device->CreateRenderPass(&desc, &renderpass_selectionOutline[1]);
			assert(success);
		}
	}

	{
		TextureDesc desc;
		desc.width = renderPath->GetRenderResult().GetDesc().width;
		desc.height = renderPath->GetRenderResult().GetDesc().height;
		desc.format = Format::D32_FLOAT;
		desc.bind_flags = BindFlag::DEPTH_STENCIL;
		desc.layout = ResourceState::DEPTHSTENCIL;
		desc.misc_flags = ResourceMiscFlag::TRANSIENT_ATTACHMENT;
		device->CreateTexture(&desc, nullptr, &editor_depthbuffer);
		device->SetName(&editor_depthbuffer, "editor_depthbuffer");

		{
			RenderPassDesc desc;
			desc.attachments.push_back(
				RenderPassAttachment::DepthStencil(
					&editor_depthbuffer,
					RenderPassAttachment::LoadOp::CLEAR,
					RenderPassAttachment::StoreOp::DONTCARE
				)
			);
			desc.attachments.push_back(
				RenderPassAttachment::RenderTarget(
					&renderPath->GetRenderResult(),
					RenderPassAttachment::LoadOp::CLEAR,
					RenderPassAttachment::StoreOp::STORE
				)
			);
			device->CreateRenderPass(&desc, &renderpass_editor);
		}
	}
}
void EditorComponent::ResizeLayout()
{
	RenderPath2D::ResizeLayout();

	// GUI elements scaling:

	float screenW = GetLogicalWidth();
	float screenH = GetLogicalHeight();

	optionsWnd.SetPos(XMFLOAT2(0, screenH - optionsWnd.GetScale().y));

	componentsWnd.SetPos(XMFLOAT2(screenW - componentsWnd.GetScale().x, screenH - componentsWnd.GetScale().y));

	////////////////////////////////////////////////////////////////////////////////////

	float hei = 25;

	saveButton.SetPos(XMFLOAT2(screenW - 40 - 44 - 44 - 104 * 3, 0));
	saveButton.SetSize(XMFLOAT2(100, hei));

	openButton.SetPos(XMFLOAT2(screenW - 40 - 44 - 44 - 104 * 2, 0));
	openButton.SetSize(XMFLOAT2(100, hei));

	closeButton.SetPos(XMFLOAT2(screenW - 40 - 44 - 44 - 104 * 1, 0));
	closeButton.SetSize(XMFLOAT2(100, hei));

	logButton.SetPos(XMFLOAT2(screenW - 40 - 44 - 44, 0));
	logButton.SetSize(XMFLOAT2(40, hei));

	aboutButton.SetPos(XMFLOAT2(screenW - 40 - 44, 0));
	aboutButton.SetSize(XMFLOAT2(40, hei));

	aboutLabel.SetSize(XMFLOAT2(screenW / 2.0f, screenH / 1.5f));
	aboutLabel.SetPos(XMFLOAT2(screenW / 2.0f - aboutLabel.scale.x / 2.0f, screenH / 2.0f - aboutLabel.scale.y / 2.0f));

	exitButton.SetPos(XMFLOAT2(screenW - 40, 0));
	exitButton.SetSize(XMFLOAT2(40, hei));
}
void EditorComponent::Load()
{
	// Font icon is from #include "FontAwesomeV6.h"
	//	We will not directly use this font style, but let the font renderer fall back on it
	//	when an icon character is not found in the default font.
	wi::font::AddFontStyle("FontAwesomeV6", font_awesome_v6, sizeof(font_awesome_v6));


	saveButton.Create(ICON_SAVE " Save");
	saveButton.font.params.shadowColor = wi::Color::Transparent();
	saveButton.SetShadowRadius(2);
	saveButton.SetTooltip("Save the current scene to a new file (Ctrl + Shift + S)");
	saveButton.SetColor(wi::Color(50, 180, 100, 180), wi::gui::WIDGETSTATE::IDLE);
	saveButton.SetColor(wi::Color(50, 220, 140, 255), wi::gui::WIDGETSTATE::FOCUS);
	saveButton.OnClick([&](wi::gui::EventArgs args) {
		SaveAs();
		});
	GetGUI().AddWidget(&saveButton);


	openButton.Create(ICON_OPEN " Open");
	openButton.SetShadowRadius(2);
	openButton.font.params.shadowColor = wi::Color::Transparent();
	openButton.SetTooltip("Open a scene, import a model or execute a Lua script...");
	openButton.SetColor(wi::Color(50, 100, 255, 180), wi::gui::WIDGETSTATE::IDLE);
	openButton.SetColor(wi::Color(120, 160, 255, 255), wi::gui::WIDGETSTATE::FOCUS);
	openButton.OnClick([&](wi::gui::EventArgs args) {
		wi::helper::FileDialogParams params;
		params.type = wi::helper::FileDialogParams::OPEN;
		params.description = ".wiscene, .obj, .gltf, .glb, .vrm, .lua";
		params.extensions.push_back("wiscene");
		params.extensions.push_back("obj");
		params.extensions.push_back("gltf");
		params.extensions.push_back("glb");
		params.extensions.push_back("vrm");
		params.extensions.push_back("lua");
		wi::helper::FileDialog(params, [&](std::string fileName) {
			wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {

				std::string extension = wi::helper::toUpper(wi::helper::GetExtensionFromFileName(fileName));
				if (!extension.compare("LUA"))
				{
					wi::lua::RunFile(fileName);
					return;
				}

				size_t camera_count_prev = GetCurrentScene().cameras.GetCount();

				main->loader.addLoadingFunction([=](wi::jobsystem::JobArgs args) {

					if (!extension.compare("WISCENE")) // engine-serialized
					{
						Scene scene;
						wi::scene::LoadModel(scene, fileName);
						GetCurrentScene().Merge(scene);
						GetCurrentEditorScene().path = fileName;
					}
					else if (!extension.compare("OBJ")) // wavefront-obj
					{
						Scene scene;
						ImportModel_OBJ(fileName, scene);
						GetCurrentScene().Merge(scene);
					}
					else if (!extension.compare("GLTF")) // text-based gltf
					{
						Scene scene;
						ImportModel_GLTF(fileName, scene);
						GetCurrentScene().Merge(scene);
					}
					else if (!extension.compare("GLB") || !extension.compare("VRM")) // binary gltf
					{
						Scene scene;
						ImportModel_GLTF(fileName, scene);
						GetCurrentScene().Merge(scene);
					}
					});
				main->loader.onFinished([=] {

					// Detect when the new scene contains a new camera, and snap the camera onto it:
					size_t camera_count = GetCurrentScene().cameras.GetCount();
					if (camera_count > 0 && camera_count > camera_count_prev)
					{
						Entity entity = GetCurrentScene().cameras.GetEntity(camera_count_prev);
						if (entity != INVALID_ENTITY)
						{
							TransformComponent* camera_transform = GetCurrentScene().transforms.GetComponent(entity);
							if (camera_transform != nullptr)
							{
								GetCurrentEditorScene().camera_transform = *camera_transform;
							}

							CameraComponent* cam = GetCurrentScene().cameras.GetComponent(entity);
							if (cam != nullptr)
							{
								GetCurrentEditorScene().camera = *cam;
								// camera aspect should be always for the current screen
								GetCurrentEditorScene().camera.width = (float)renderPath->GetInternalResolution().x;
								GetCurrentEditorScene().camera.height = (float)renderPath->GetInternalResolution().y;
							}
						}
					}

					main->ActivatePath(this, 0.2f, wi::Color::Black());
					componentsWnd.weatherWnd.Update();
					optionsWnd.RefreshEntityTree();
					RefreshSceneList();
					});
				main->ActivatePath(&main->loader, 0.2f, wi::Color::Black());
				ResetHistory();
				});
			});
		});
	GetGUI().AddWidget(&openButton);


	closeButton.Create(ICON_CLOSE " Close");
	closeButton.SetShadowRadius(2);
	closeButton.font.params.shadowColor = wi::Color::Transparent();
	closeButton.SetTooltip("Close the current scene.\nThis will clear everything from the currently selected scene, and delete the scene.\nThis operation cannot be undone!");
	closeButton.SetColor(wi::Color(255, 130, 100, 180), wi::gui::WIDGETSTATE::IDLE);
	closeButton.SetColor(wi::Color(255, 200, 150, 255), wi::gui::WIDGETSTATE::FOCUS);
	closeButton.OnClick([&](wi::gui::EventArgs args) {

		optionsWnd.terragen.Generation_Cancel();
		optionsWnd.terragen.terrainEntity = INVALID_ENTITY;
		optionsWnd.terragen.SetCollapsed(true);

		translator.selected.clear();
		wi::scene::Scene& scene = GetCurrentScene();
		wi::renderer::ClearWorld(scene);
		optionsWnd.cameraWnd.SetEntity(INVALID_ENTITY);
		optionsWnd.paintToolWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.objectWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.meshWnd.SetEntity(INVALID_ENTITY, -1);
		componentsWnd.lightWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.soundWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.decalWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.envProbeWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.materialWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.emitterWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.hairWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.forceFieldWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.springWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.ikWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.transformWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.layerWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.nameWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.animWnd.SetEntity(INVALID_ENTITY);

		optionsWnd.RefreshEntityTree();
		ResetHistory();
		GetCurrentEditorScene().path.clear();

		wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
			if (scenes.size() > 1)
			{
				scenes.erase(scenes.begin() + current_scene);
			}
			SetCurrentScene(std::max(0, current_scene - 1));
			});
		});
	GetGUI().AddWidget(&closeButton);


	logButton.Create(ICON_BACKLOG);
	logButton.SetShadowRadius(2);
	logButton.font.params.shadowColor = wi::Color::Transparent();
	logButton.SetTooltip("Open the backlog (toggle with HOME button)");
	logButton.SetColor(wi::Color(50, 160, 200, 180), wi::gui::WIDGETSTATE::IDLE);
	logButton.SetColor(wi::Color(120, 200, 200, 255), wi::gui::WIDGETSTATE::FOCUS);
	logButton.OnClick([&](wi::gui::EventArgs args) {
		wi::backlog::Toggle();
		});
	GetGUI().AddWidget(&logButton);


	aboutButton.Create(ICON_HELP);
	aboutButton.SetShadowRadius(2);
	aboutButton.font.params.shadowColor = wi::Color::Transparent();
	aboutButton.SetTooltip("About...");
	aboutButton.SetColor(wi::Color(50, 160, 200, 180), wi::gui::WIDGETSTATE::IDLE);
	aboutButton.SetColor(wi::Color(120, 200, 200, 255), wi::gui::WIDGETSTATE::FOCUS);
	aboutButton.OnClick([&](wi::gui::EventArgs args) {
		aboutLabel.SetVisible(!aboutLabel.IsVisible());
		});
	GetGUI().AddWidget(&aboutButton);

	{
		std::string ss;
		ss += "Wicked Engine Editor v";
		ss += wi::version::GetVersionString();
		ss += "\nCreated by Turánszki János";
		ss += "\n\nWebsite: https://wickedengine.net/";
		ss += "\nSource code: https://github.com/turanszkij/WickedEngine";
		ss += "\nDiscord chat: https://discord.gg/CFjRYmE";
		ss += "\n\nControls\n";
		ss += "------------\n";
		ss += "Move camera: WASD, or Contoller left stick or D-pad\n";
		ss += "Look: Middle mouse button / arrow keys / controller right stick\n";
		ss += "Select: Right mouse button\n";
		ss += "Interact with water: Left mouse button when nothing is selected\n";
		ss += "Faster camera: Left Shift button or controller R2/RT\n";
		ss += "Snap transform: Left Ctrl (hold while transforming)\n";
		ss += "Camera up: E\n";
		ss += "Camera down: Q\n";
		ss += "Duplicate entity: Ctrl + D\n";
		ss += "Select All: Ctrl + A\n";
		ss += "Deselect All: Esc\n";
		ss += "Undo: Ctrl + Z\n";
		ss += "Redo: Ctrl + Y\n";
		ss += "Copy: Ctrl + C\n";
		ss += "Cut: Ctrl + X\n";
		ss += "Paste: Ctrl + V\n";
		ss += "Delete: Delete button\n";
		ss += "Save As: Ctrl + Shift + S\n";
		ss += "Save: Ctrl + S\n";
		ss += "Transform: Ctrl + T\n";
		ss += "Wireframe mode: Ctrl + W\n";
		ss += "Color grading reference: Ctrl + G (color grading palette reference will be displayed in top left corner)\n";
		ss += "Inspector mode: I button (hold), hovered entity information will be displayed near mouse position.\n";
		ss += "Place Instances: Ctrl + Shift + Left mouse click (place clipboard onto clicked surface)\n";
		ss += "Script Console / backlog: HOME button\n";
		ss += "Bone picking: First select an armature, only then bone picking becomes available. As long as you have an armature or bone selected, bone picking will remain active.\n";
		ss += "\n";
		ss += "\nTips\n";
		ss += "-------\n";
		ss += "You can find sample scenes in the Content/models directory. Try to load one.\n";
		ss += "You can also import models from .OBJ, .GLTF, .GLB, .VRM files.\n";
#ifndef PLATFORM_UWP
		ss += "You can find a program configuration file at Editor/config.ini\n";
#endif // PLATFORM_UWP
		ss += "You can find sample LUA scripts in the Content/scripts directory. Try to load one.\n";
		ss += "You can find a startup script at Editor/startup.lua (this will be executed on program start, if exists)\n";
		ss += "\nFor questions, bug reports, feedback, requests, please open an issue at:\n";
		ss += "https://github.com/turanszkij/WickedEngine/issues\n";

		aboutLabel.Create("AboutLabel");
		aboutLabel.SetText(ss);
		aboutLabel.SetVisible(false);
		aboutLabel.SetColor(wi::Color(113, 183, 214, 100));
		GetGUI().AddWidget(&aboutLabel);
	}

	exitButton.Create(ICON_EXIT);
	exitButton.SetShadowRadius(2);
	exitButton.font.params.shadowColor = wi::Color::Transparent();
	exitButton.SetTooltip("Exit");
	exitButton.SetColor(wi::Color(160, 50, 50, 180), wi::gui::WIDGETSTATE::IDLE);
	exitButton.SetColor(wi::Color(200, 50, 50, 255), wi::gui::WIDGETSTATE::FOCUS);
	exitButton.OnClick([this](wi::gui::EventArgs args) {
		optionsWnd.terragen.Generation_Cancel();
		wi::platform::Exit();
		});
	GetGUI().AddWidget(&exitButton);

	optionsWnd.Create(this);
	GetGUI().AddWidget(&optionsWnd);

	componentsWnd.Create(this);
	GetGUI().AddWidget(&componentsWnd);

	optionsWnd.themeCombo.SetSelected(0);

	static wi::eventhandler::Handle handle = wi::eventhandler::Subscribe(TerrainGenerator::EVENT_THEME_RESET, [=](uint64_t) {
		optionsWnd.themeCombo.SetSelected(optionsWnd.themeCombo.GetSelected());
		});

	RenderPath2D::Load();
}
void EditorComponent::Start()
{
	RenderPath2D::Start();
}
void EditorComponent::PreUpdate()
{
	RenderPath2D::PreUpdate();

	renderPath->PreUpdate();
}
void EditorComponent::FixedUpdate()
{
	RenderPath2D::FixedUpdate();

	renderPath->FixedUpdate();
}
void EditorComponent::Update(float dt)
{
	wi::profiler::range_id profrange = wi::profiler::BeginRangeCPU("Editor Update");

	Scene& scene = GetCurrentScene();
	EditorScene& editorscene = GetCurrentEditorScene();
	CameraComponent& camera = editorscene.camera;

	optionsWnd.terragen.scene = &scene;
	translator.scene = &scene;

	if (scene.forces.Contains(grass_interaction_entity))
	{
		scene.Entity_Remove(grass_interaction_entity);
	}

	optionsWnd.Update(dt);
	componentsWnd.Update(dt);

	// Pulsating selection color update:
	selectionOutlineTimer += dt;

	CheckBonePickingEnabled();

	bool clear_selected = false;
	if (wi::input::Press(wi::input::KEYBOARD_BUTTON_ESCAPE))
	{
		if (optionsWnd.cinemaModeCheckBox.GetCheck())
		{
			// Exit cinema mode:
			if (renderPath != nullptr)
			{
				renderPath->GetGUI().SetVisible(true);
			}
			GetGUI().SetVisible(true);
			wi::profiler::SetEnabled(optionsWnd.profilerEnabledCheckBox.GetCheck());

			optionsWnd.cinemaModeCheckBox.SetCheck(false);
		}
		else
		{
			clear_selected = true;
		}
	}

	translator.interactable = false;
	bool deleting = false;

	// Camera control:
	if (!wi::backlog::isActive() && !GetGUI().HasFocus())
	{
		deleting = wi::input::Press(wi::input::KEYBOARD_BUTTON_DELETE);
		translator.interactable = true;
		XMFLOAT4 currentMouse = wi::input::GetPointer();
		static XMFLOAT4 originalMouse = XMFLOAT4(0, 0, 0, 0);
		static bool camControlStart = true;
		if (camControlStart)
		{
			originalMouse = wi::input::GetPointer();
		}

		float xDif = 0, yDif = 0;

		if (wi::input::Down(wi::input::MOUSE_BUTTON_MIDDLE))
		{
			camControlStart = false;
#if 0
			// Mouse delta from previous frame:
			xDif = currentMouse.x - originalMouse.x;
			yDif = currentMouse.y - originalMouse.y;
#else
			// Mouse delta from hardware read:
			xDif = wi::input::GetMouseState().delta_position.x;
			yDif = wi::input::GetMouseState().delta_position.y;
#endif
			xDif = 0.1f * xDif * (1.0f / 60.0f);
			yDif = 0.1f * yDif * (1.0f / 60.0f);
			wi::input::SetPointer(originalMouse);
			wi::input::HidePointer(true);
	}
		else
		{
			camControlStart = true;
			wi::input::HidePointer(false);
		}

		const float buttonrotSpeed = 2.0f * dt;
		if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LEFT))
		{
			xDif -= buttonrotSpeed;
		}
		if (wi::input::Down(wi::input::KEYBOARD_BUTTON_RIGHT))
		{
			xDif += buttonrotSpeed;
		}
		if (wi::input::Down(wi::input::KEYBOARD_BUTTON_UP))
		{
			yDif -= buttonrotSpeed;
		}
		if (wi::input::Down(wi::input::KEYBOARD_BUTTON_DOWN))
		{
			yDif += buttonrotSpeed;
		}

		const XMFLOAT4 leftStick = wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_L, 0);
		const XMFLOAT4 rightStick = wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_THUMBSTICK_R, 0);
		const XMFLOAT4 rightTrigger = wi::input::GetAnalog(wi::input::GAMEPAD_ANALOG_TRIGGER_R, 0);

		const float jostickrotspeed = 0.05f;
		xDif += rightStick.x * jostickrotspeed;
		yDif += rightStick.y * jostickrotspeed;

		xDif *= optionsWnd.cameraWnd.rotationspeedSlider.GetValue();
		yDif *= optionsWnd.cameraWnd.rotationspeedSlider.GetValue();


		if (optionsWnd.cameraWnd.fpsCheckBox.GetCheck())
		{
			// FPS Camera
			const float clampedDT = std::min(dt, 0.1f); // if dt > 100 millisec, don't allow the camera to jump too far...

			const float speed = ((wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT) ? 10.0f : 1.0f) + rightTrigger.x * 10.0f) * optionsWnd.cameraWnd.movespeedSlider.GetValue() * clampedDT;
			XMVECTOR move = XMLoadFloat3(&editorscene.cam_move);
			XMVECTOR moveNew = XMVectorSet(leftStick.x, 0, leftStick.y, 0);

			if (!wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL))
			{
				// Only move camera if control not pressed
				if (wi::input::Down((wi::input::BUTTON)'A') || wi::input::Down(wi::input::GAMEPAD_BUTTON_LEFT)) { moveNew += XMVectorSet(-1, 0, 0, 0); }
				if (wi::input::Down((wi::input::BUTTON)'D') || wi::input::Down(wi::input::GAMEPAD_BUTTON_RIGHT)) { moveNew += XMVectorSet(1, 0, 0, 0); }
				if (wi::input::Down((wi::input::BUTTON)'W') || wi::input::Down(wi::input::GAMEPAD_BUTTON_UP)) { moveNew += XMVectorSet(0, 0, 1, 0); }
				if (wi::input::Down((wi::input::BUTTON)'S') || wi::input::Down(wi::input::GAMEPAD_BUTTON_DOWN)) { moveNew += XMVectorSet(0, 0, -1, 0); }
				if (wi::input::Down((wi::input::BUTTON)'E') || wi::input::Down(wi::input::GAMEPAD_BUTTON_2)) { moveNew += XMVectorSet(0, 1, 0, 0); }
				if (wi::input::Down((wi::input::BUTTON)'Q') || wi::input::Down(wi::input::GAMEPAD_BUTTON_1)) { moveNew += XMVectorSet(0, -1, 0, 0); }
				moveNew += XMVector3Normalize(moveNew);
			}
			moveNew *= speed;

			move = XMVectorLerp(move, moveNew, optionsWnd.cameraWnd.accelerationSlider.GetValue() * clampedDT / 0.0166f); // smooth the movement a bit
			float moveLength = XMVectorGetX(XMVector3Length(move));

			if (moveLength < 0.0001f)
			{
				move = XMVectorSet(0, 0, 0, 0);
			}

			if (abs(xDif) + abs(yDif) > 0 || moveLength > 0.0001f)
			{
				XMMATRIX camRot = XMMatrixRotationQuaternion(XMLoadFloat4(&editorscene.camera_transform.rotation_local));
				XMVECTOR move_rot = XMVector3TransformNormal(move, camRot);
				XMFLOAT3 _move;
				XMStoreFloat3(&_move, move_rot);
				editorscene.camera_transform.Translate(_move);
				editorscene.camera_transform.RotateRollPitchYaw(XMFLOAT3(yDif, xDif, 0));
				camera.SetDirty();
			}

			editorscene.camera_transform.UpdateTransform();
			XMStoreFloat3(&editorscene.cam_move, move);
		}
		else
		{
			// Orbital Camera

			if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT))
			{
				XMVECTOR V = XMVectorAdd(camera.GetRight() * xDif, camera.GetUp() * yDif) * 10;
				XMFLOAT3 vec;
				XMStoreFloat3(&vec, V);
				editorscene.camera_target.Translate(vec);
			}
			else if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL) || currentMouse.z != 0.0f)
			{
				editorscene.camera_transform.Translate(XMFLOAT3(0, 0, yDif * 4 + currentMouse.z));
				editorscene.camera_transform.translation_local.z = std::min(0.0f, editorscene.camera_transform.translation_local.z);
				camera.SetDirty();
			}
			else if (abs(xDif) + abs(yDif) > 0)
			{
				editorscene.camera_target.RotateRollPitchYaw(XMFLOAT3(yDif * 2, xDif * 2, 0));
				camera.SetDirty();
			}

			editorscene.camera_target.UpdateTransform();
			editorscene.camera_transform.UpdateTransform_Parented(editorscene.camera_target);
		}

		inspector_mode = wi::input::Down((wi::input::BUTTON)'I');

		// Begin picking:
		unsigned int pickMask = optionsWnd.rendererWnd.GetPickType();
		Ray pickRay = wi::renderer::GetPickRay((long)currentMouse.x, (long)currentMouse.y, *this, camera);
		{
			hovered = wi::scene::PickResult();

			if (pickMask & PICK_LIGHT)
			{
				for (size_t i = 0; i < scene.lights.GetCount(); ++i)
				{
					Entity entity = scene.lights.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_DECAL)
			{
				for (size_t i = 0; i < scene.decals.GetCount(); ++i)
				{
					Entity entity = scene.decals.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_FORCEFIELD)
			{
				for (size_t i = 0; i < scene.forces.GetCount(); ++i)
				{
					Entity entity = scene.forces.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_EMITTER)
			{
				for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
				{
					Entity entity = scene.emitters.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_HAIR)
			{
				for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
				{
					Entity entity = scene.hairs.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_ENVPROBE)
			{
				for (size_t i = 0; i < scene.probes.GetCount(); ++i)
				{
					Entity entity = scene.probes.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					if (Sphere(transform.GetPosition(), 1).intersects(pickRay))
					{
						float dis = wi::math::Distance(transform.GetPosition(), pickRay.origin);
						if (dis < hovered.distance)
						{
							hovered = wi::scene::PickResult();
							hovered.entity = entity;
							hovered.distance = dis;
						}
					}
				}
			}
			if (pickMask & PICK_CAMERA)
			{
				for (size_t i = 0; i < scene.cameras.GetCount(); ++i)
				{
					Entity entity = scene.cameras.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_ARMATURE)
			{
				for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
				{
					Entity entity = scene.armatures.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (pickMask & PICK_SOUND)
			{
				for (size_t i = 0; i < scene.sounds.GetCount(); ++i)
				{
					Entity entity = scene.sounds.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					XMVECTOR disV = XMVector3LinePointDistance(XMLoadFloat3(&pickRay.origin), XMLoadFloat3(&pickRay.origin) + XMLoadFloat3(&pickRay.direction), transform.GetPositionV());
					float dis = XMVectorGetX(disV);
					if (dis > 0.01f && dis < wi::math::Distance(transform.GetPosition(), pickRay.origin) * 0.05f && dis < hovered.distance)
					{
						hovered = wi::scene::PickResult();
						hovered.entity = entity;
						hovered.distance = dis;
					}
				}
			}
			if (bone_picking)
			{
				for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
				{
					const ArmatureComponent& armature = scene.armatures[i];
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
						XMVECTOR ab = XMVector3Normalize(b - a);

						wi::primitive::Capsule capsule;
						capsule.radius = wi::math::Distance(a, b) * 0.1f;
						capsule.radius = wi::math::Clamp(capsule.radius, 0.01f, 0.1f);
						a -= ab * capsule.radius;
						b += ab * capsule.radius;
						XMStoreFloat3(&capsule.base, a);
						XMStoreFloat3(&capsule.tip, b);

						float dis = -1;
						if (pickRay.intersects(capsule, dis) && dis < hovered.distance)
						{
							hovered = wi::scene::PickResult();
							hovered.entity = entity;
							hovered.distance = dis;
						}
					}
				}
			}

			if ((pickMask & PICK_OBJECT) && hovered.entity == INVALID_ENTITY)
			{
				// Object picking only when mouse button down, because it can be slow with high polycount
				if (
					wi::input::Down(wi::input::MOUSE_BUTTON_LEFT) ||
					wi::input::Down(wi::input::MOUSE_BUTTON_RIGHT) ||
					optionsWnd.paintToolWnd.GetMode() != PaintToolWindow::MODE_DISABLED ||
					inspector_mode
					)
				{
					hovered = wi::scene::Pick(pickRay, pickMask, ~0u, scene);
				}
			}
		}

		// Interactions only when paint tool is disabled:
		if (optionsWnd.paintToolWnd.GetMode() == PaintToolWindow::MODE_DISABLED)
		{
			// Interact:
			if (hovered.entity != INVALID_ENTITY && wi::input::Down(wi::input::MOUSE_BUTTON_LEFT))
			{
				const ObjectComponent* object = scene.objects.GetComponent(hovered.entity);
				if (object != nullptr)
				{	
					if (translator.selected.empty() && object->GetRenderTypes() & wi::enums::RENDERTYPE_WATER)
					{
						// if water, then put a water ripple onto it:
						scene.PutWaterRipple("images/ripple.png", hovered.position);
					}
					else if (componentsWnd.decalWnd.IsEnabled() && componentsWnd.decalWnd.placementCheckBox.GetCheck() && wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
					{
						// if not water, put a decal on it:
						Entity entity = scene.Entity_CreateDecal("editorDecal", "");
						// material and decal parameters will be copied from selected:
						if (scene.decals.Contains(componentsWnd.decalWnd.entity))
						{
							*scene.decals.GetComponent(entity) = *scene.decals.GetComponent(componentsWnd.decalWnd.entity);
						}
						if (scene.materials.Contains(componentsWnd.decalWnd.entity))
						{
							*scene.materials.GetComponent(entity) = *scene.materials.GetComponent(componentsWnd.decalWnd.entity);
						}
						// place it on picked surface:
						TransformComponent& transform = *scene.transforms.GetComponent(entity);
						transform.MatrixTransform(hovered.orientation);
						transform.RotateRollPitchYaw(XMFLOAT3(XM_PIDIV2, 0, 0));
						scene.Component_Attach(entity, hovered.entity);

						wi::Archive& archive = AdvanceHistory();
						archive << EditorComponent::HISTORYOP_ADD;
						RecordSelection(archive);
						RecordSelection(archive);
						RecordEntity(archive, entity);

						optionsWnd.RefreshEntityTree();
					}
					else
					{
						// Check for interactive grass (hair particle that is child of hovered object:
						for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
						{
							Entity entity = scene.hairs.GetEntity(i);
							HierarchyComponent* hier = scene.hierarchy.GetComponent(entity);
							if (hier != nullptr && hier->parentID == hovered.entity)
							{
								XMVECTOR P = XMLoadFloat3(&hovered.position);
								P += XMLoadFloat3(&hovered.normal) * 2;
								if (grass_interaction_entity == INVALID_ENTITY)
								{
									grass_interaction_entity = CreateEntity();
								}
								ForceFieldComponent& force = scene.forces.Create(grass_interaction_entity);
								TransformComponent& transform = scene.transforms.Create(grass_interaction_entity);
								force.type = ENTITY_TYPE_FORCEFIELD_POINT;
								force.gravity = -80;
								force.range = 3;
								transform.Translate(P);
								break;
							}
						}
					}
				}

			}
		}

		// Select...
		if (wi::input::Press(wi::input::MOUSE_BUTTON_RIGHT) || selectAll || clear_selected)
		{

			wi::Archive& archive = AdvanceHistory();
			archive << HISTORYOP_SELECTION;
			// record PREVIOUS selection state...
			RecordSelection(archive);

			if (selectAll)
			{
				// Add everything to selection:
				selectAll = false;
				ClearSelected();

				selectAllStorage.clear();
				scene.FindAllEntities(selectAllStorage);
				for (auto& entity : selectAllStorage)
				{
					AddSelected(entity);
				}
			}
			else if (hovered.entity != INVALID_ENTITY)
			{
				// Add the hovered item to the selection:

				if (!translator.selected.empty() && wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT))
				{
					// Union selection:
					wi::vector<wi::scene::PickResult> saved = translator.selected;
					translator.selected.clear(); 
					for (const wi::scene::PickResult& picked : saved)
					{
						AddSelected(picked);
					}
					AddSelected(hovered);
				}
				else
				{
					// Replace selection:
					translator.selected.clear();
					AddSelected(hovered);
				}
			}
			else
			{
				clear_selected = true;
			}

			if (clear_selected)
			{
				ClearSelected();
			}


			// record NEW selection state...
			RecordSelection(archive);

			optionsWnd.RefreshEntityTree();
		}

	}

	main->infoDisplay.colorgrading_helper = false;

	// Control operations...
	if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LCONTROL))
	{
		// Color Grading helper
		if (wi::input::Down((wi::input::BUTTON)'G'))
		{
			main->infoDisplay.colorgrading_helper = true;
		}
		// Toggle wireframe mode
		if (wi::input::Press((wi::input::BUTTON)'W'))
		{
			wi::renderer::SetWireRender(!wi::renderer::IsWireRender());
			optionsWnd.rendererWnd.wireFrameCheckBox.SetCheck(wi::renderer::IsWireRender());
		}
		// Enable transform tool
		if (wi::input::Press((wi::input::BUTTON)'T'))
		{
			translator.SetEnabled(!translator.IsEnabled());
			optionsWnd.isTranslatorCheckBox.SetCheck(translator.isTranslator);
			optionsWnd.isRotatorCheckBox.SetCheck(translator.isRotator);
			optionsWnd.isScalatorCheckBox.SetCheck(translator.isScalator);
		}
		// Save
		if (wi::input::Press((wi::input::BUTTON)'S'))
		{
			if (wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT) || GetCurrentEditorScene().path.empty())
			{
				SaveAs();
			}
			else
			{
				Save(GetCurrentEditorScene().path);
			}
		}
		// Select All
		if (wi::input::Press((wi::input::BUTTON)'A'))
		{
			selectAll = true;
		}
		// Copy/Cut
		if (wi::input::Press((wi::input::BUTTON)'C') || wi::input::Press((wi::input::BUTTON)'X'))
		{
			auto& prevSel = translator.selectedEntitiesNonRecursive;

			EntitySerializer seri;
			clipboard.SetReadModeAndResetPos(false);
			clipboard << prevSel.size();
			for (auto& x : prevSel)
			{
				scene.Entity_Serialize(clipboard, seri, x);
			}

			if (wi::input::Press((wi::input::BUTTON)'X'))
			{
				deleting = true;
			}
		}
		// Paste
		if (wi::input::Press((wi::input::BUTTON)'V'))
		{
			wi::Archive& archive = AdvanceHistory();
			archive << HISTORYOP_ADD;
			RecordSelection(archive);

			ClearSelected();

			EntitySerializer seri;
			clipboard.SetReadModeAndResetPos(true);
			size_t count;
			clipboard >> count;
			wi::vector<Entity> addedEntities;
			for (size_t i = 0; i < count; ++i)
			{
				wi::scene::PickResult picked;
				picked.entity = scene.Entity_Serialize(clipboard, seri, INVALID_ENTITY, Scene::EntitySerializeFlags::RECURSIVE);
				AddSelected(picked);
				addedEntities.push_back(picked.entity);
			}

			RecordSelection(archive);
			RecordEntity(archive, addedEntities);

			optionsWnd.RefreshEntityTree();
		}
		// Duplicate Instances
		if (wi::input::Press((wi::input::BUTTON)'D'))
		{
			wi::Archive& archive = AdvanceHistory();
			archive << HISTORYOP_ADD;
			RecordSelection(archive);

			auto& prevSel = translator.selectedEntitiesNonRecursive;
			wi::vector<Entity> addedEntities;
			for (auto& x : prevSel)
			{
				wi::scene::PickResult picked;
				picked.entity = scene.Entity_Duplicate(x);
				addedEntities.push_back(picked.entity);
			}

			ClearSelected();

			for (auto& x : addedEntities)
			{
				AddSelected(x);
			}

			RecordSelection(archive);
			RecordEntity(archive, addedEntities);

			optionsWnd.RefreshEntityTree();
		}
		// Put Instances
		if (clipboard.IsOpen() && hovered.subsetIndex >= 0 && wi::input::Down(wi::input::KEYBOARD_BUTTON_LSHIFT) && wi::input::Press(wi::input::MOUSE_BUTTON_LEFT))
		{
			wi::vector<Entity> addedEntities;
			EntitySerializer seri;
			clipboard.SetReadModeAndResetPos(true);
			size_t count;
			clipboard >> count;
			for (size_t i = 0; i < count; ++i)
			{
				Entity entity = scene.Entity_Serialize(clipboard, seri, INVALID_ENTITY, Scene::EntitySerializeFlags::RECURSIVE | Scene::EntitySerializeFlags::KEEP_INTERNAL_ENTITY_REFERENCES);
				const HierarchyComponent* hier = scene.hierarchy.GetComponent(entity);
				if (hier != nullptr)
				{
					scene.Component_Detach(entity);
				}
				TransformComponent* transform = scene.transforms.GetComponent(entity);
				if (transform != nullptr)
				{
					transform->translation_local = {};
#if 0
					// orient around surface normal:
					transform->MatrixTransform(hovered.orientation);
#else
					// orient in random vertical rotation only:
					transform->RotateRollPitchYaw(XMFLOAT3(0, wi::random::GetRandom(XM_PI), 0));
					transform->Translate(hovered.position);
#endif
					transform->UpdateTransform();
				}
				if (hier != nullptr)
				{
					scene.Component_Attach(entity, hier->parentID);
				}
				addedEntities.push_back(entity);
			}

			wi::Archive& archive = AdvanceHistory();
			archive << HISTORYOP_ADD;
			// because selection didn't change here, we record same selection state twice, it's not a bug:
			RecordSelection(archive);
			RecordSelection(archive);
			RecordEntity(archive, addedEntities);

			optionsWnd.RefreshEntityTree();
	}
		// Undo
		if (wi::input::Press((wi::input::BUTTON)'Z'))
		{
			ConsumeHistoryOperation(true);

			optionsWnd.RefreshEntityTree();
		}
		// Redo
		if (wi::input::Press((wi::input::BUTTON)'Y'))
		{
			ConsumeHistoryOperation(false);

			optionsWnd.RefreshEntityTree();
		}
		}

	// Delete
	if (deleting)
	{
		wi::Archive& archive = AdvanceHistory();
		archive << HISTORYOP_DELETE;
		RecordSelection(archive);

		archive << translator.selectedEntitiesNonRecursive;
		EntitySerializer seri;
		for (auto& x : translator.selectedEntitiesNonRecursive)
		{
			scene.Entity_Serialize(archive, seri, x);
		}
		for (auto& x : translator.selectedEntitiesNonRecursive)
		{
			scene.Entity_Remove(x);
		}

		ClearSelected();

		optionsWnd.RefreshEntityTree();
	}

	// Update window data bindings...
	if (translator.selected.empty())
	{
		optionsWnd.cameraWnd.SetEntity(INVALID_ENTITY);
		optionsWnd.paintToolWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.objectWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.emitterWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.hairWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.meshWnd.SetEntity(INVALID_ENTITY, -1);
		componentsWnd.materialWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.soundWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.decalWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.envProbeWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.forceFieldWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.springWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.ikWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.transformWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.layerWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.nameWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.weatherWnd.SetEntity(INVALID_ENTITY);
		componentsWnd.animWnd.SetEntity(INVALID_ENTITY);
	}
	else
	{
		const wi::scene::PickResult& picked = translator.selected.back();

		assert(picked.entity != INVALID_ENTITY);
		componentsWnd.objectWnd.SetEntity(picked.entity);

		for (auto& x : translator.selected)
		{
			if (scene.objects.GetComponent(x.entity) != nullptr)
			{
				componentsWnd.objectWnd.SetEntity(x.entity);
				break;
			}
		}

		optionsWnd.cameraWnd.SetEntity(picked.entity);
		optionsWnd.paintToolWnd.SetEntity(picked.entity, picked.subsetIndex);
		componentsWnd.emitterWnd.SetEntity(picked.entity);
		componentsWnd.hairWnd.SetEntity(picked.entity);
		componentsWnd.lightWnd.SetEntity(picked.entity);
		componentsWnd.soundWnd.SetEntity(picked.entity);
		componentsWnd.decalWnd.SetEntity(picked.entity);
		componentsWnd.envProbeWnd.SetEntity(picked.entity);
		componentsWnd.forceFieldWnd.SetEntity(picked.entity);
		componentsWnd.springWnd.SetEntity(picked.entity);
		componentsWnd.ikWnd.SetEntity(picked.entity);
		componentsWnd.transformWnd.SetEntity(picked.entity);
		componentsWnd.layerWnd.SetEntity(picked.entity);
		componentsWnd.nameWnd.SetEntity(picked.entity);
		componentsWnd.weatherWnd.SetEntity(picked.entity);
		componentsWnd.animWnd.SetEntity(picked.entity);

		if (picked.subsetIndex >= 0)
		{
			const ObjectComponent* object = scene.objects.GetComponent(picked.entity);
			if (object != nullptr) // maybe it was deleted...
			{
				componentsWnd.meshWnd.SetEntity(object->meshID, picked.subsetIndex);

				const MeshComponent* mesh = scene.meshes.GetComponent(object->meshID);
				if (mesh != nullptr && (int)mesh->subsets.size() > picked.subsetIndex)
				{
					componentsWnd.materialWnd.SetEntity(mesh->subsets[picked.subsetIndex].materialID);
				}
			}
		}
		else
		{
			componentsWnd.meshWnd.SetEntity(picked.entity, picked.subsetIndex);
			componentsWnd.materialWnd.SetEntity(picked.entity);
		}

	}

	// Clear highlight state:
	for (size_t i = 0; i < scene.materials.GetCount(); ++i)
	{
		scene.materials[i].SetUserStencilRef(EDITORSTENCILREF_CLEAR);
	}
	for (size_t i = 0; i < scene.objects.GetCount(); ++i)
	{
		scene.objects[i].SetUserStencilRef(EDITORSTENCILREF_CLEAR);
	}
	for (auto& x : translator.selected)
	{
		ObjectComponent* object = scene.objects.GetComponent(x.entity);
		if (object != nullptr) // maybe it was deleted...
		{
			object->SetUserStencilRef(EDITORSTENCILREF_HIGHLIGHT_OBJECT);
			if (x.subsetIndex >= 0)
			{
				const MeshComponent* mesh = scene.meshes.GetComponent(object->meshID);
				if (mesh != nullptr && (int)mesh->subsets.size() > x.subsetIndex)
				{
					MaterialComponent* material = scene.materials.GetComponent(mesh->subsets[x.subsetIndex].materialID);
					if (material != nullptr)
					{
						material->SetUserStencilRef(EDITORSTENCILREF_HIGHLIGHT_MATERIAL);
					}
				}
			}
		}
	}

	if (translator.IsDragEnded())
	{
		EntitySerializer seri;
		wi::Archive& archive = AdvanceHistory();
		archive << HISTORYOP_TRANSLATOR;
		archive << translator.isTranslator;
		archive << translator.isRotator;
		archive << translator.isScalator;
		translator.transform_start.Serialize(archive, seri);
		translator.transform.Serialize(archive, seri);
		archive << translator.matrices_start;
		archive << translator.matrices_current;
	}

	componentsWnd.emitterWnd.UpdateData();
	componentsWnd.hairWnd.UpdateData();

	// Follow camera proxy:
	if (optionsWnd.cameraWnd.followCheckBox.IsEnabled() && optionsWnd.cameraWnd.followCheckBox.GetCheck())
	{
		TransformComponent* proxy = scene.transforms.GetComponent(optionsWnd.cameraWnd.entity);
		if (proxy != nullptr)
		{
			editorscene.camera_transform.Lerp(editorscene.camera_transform, *proxy, 1.0f - optionsWnd.cameraWnd.followSlider.GetValue());
			editorscene.camera_transform.UpdateTransform();
		}
	}

	camera.TransformCamera(editorscene.camera_transform);
	camera.UpdateCamera();

	wi::RenderPath3D_PathTracing* pathtracer = dynamic_cast<wi::RenderPath3D_PathTracing*>(renderPath.get());
	if (pathtracer != nullptr)
	{
		pathtracer->setTargetSampleCount((int)optionsWnd.pathTraceTargetSlider.GetValue());

		std::string ss;
		ss += "Sample count: " + std::to_string(pathtracer->getCurrentSampleCount()) + "\n";
		ss += "Trace progress: " + std::to_string(int(pathtracer->getProgress() * 100)) + "%\n";
		if (pathtracer->isDenoiserAvailable())
		{
			if (pathtracer->getDenoiserProgress() > 0)
			{
				ss += "Denoiser progress: " + std::to_string(int(pathtracer->getDenoiserProgress() * 100)) + "%\n";
			}
		}
		else
		{
			ss += "Denoiser not available!\nTo find out how to enable the denoiser, visit the documentation.";
		}
		optionsWnd.pathTraceStatisticsLabel.SetText(ss);
	}

	optionsWnd.terragen.Generation_Update(camera);

	wi::profiler::EndRange(profrange);

	RenderPath2D::Update(dt);

	if (optionsWnd.paintToolWnd.GetMode() == PaintToolWindow::MODE::MODE_DISABLED)
	{
		translator.Update(camera, *this);
	}

	renderPath->colorspace = colorspace;
	renderPath->Update(dt);
}
void EditorComponent::PostUpdate()
{
	RenderPath2D::PostUpdate();

	renderPath->PostUpdate();
}
void EditorComponent::Render() const
{
	const Scene& scene = GetCurrentScene();

	// Hovered item boxes:
	if (!optionsWnd.cinemaModeCheckBox.GetCheck())
	{
		if (hovered.entity != INVALID_ENTITY)
		{
			const ObjectComponent* object = scene.objects.GetComponent(hovered.entity);
			if (object != nullptr)
			{
				const AABB& aabb = *scene.aabb_objects.GetComponent(hovered.entity);

				XMFLOAT4X4 hoverBox;
				XMStoreFloat4x4(&hoverBox, aabb.getAsBoxMatrix());
				wi::renderer::DrawBox(hoverBox, XMFLOAT4(0.5f, 0.5f, 0.5f, 0.5f));
			}

			const LightComponent* light = scene.lights.GetComponent(hovered.entity);
			if (light != nullptr)
			{
				const AABB& aabb = *scene.aabb_lights.GetComponent(hovered.entity);

				XMFLOAT4X4 hoverBox;
				XMStoreFloat4x4(&hoverBox, aabb.getAsBoxMatrix());
				wi::renderer::DrawBox(hoverBox, XMFLOAT4(0.5f, 0.5f, 0, 0.5f));
			}

			const DecalComponent* decal = scene.decals.GetComponent(hovered.entity);
			if (decal != nullptr)
			{
				wi::renderer::DrawBox(decal->world, XMFLOAT4(0.5f, 0, 0.5f, 0.5f));
			}

			const EnvironmentProbeComponent* probe = scene.probes.GetComponent(hovered.entity);
			if (probe != nullptr)
			{
				const AABB& aabb = *scene.aabb_probes.GetComponent(hovered.entity);

				XMFLOAT4X4 hoverBox;
				XMStoreFloat4x4(&hoverBox, aabb.getAsBoxMatrix());
				wi::renderer::DrawBox(hoverBox, XMFLOAT4(0.5f, 0.5f, 0.5f, 0.5f));
			}

			const wi::HairParticleSystem* hair = scene.hairs.GetComponent(hovered.entity);
			if (hair != nullptr)
			{
				XMFLOAT4X4 hoverBox;
				XMStoreFloat4x4(&hoverBox, hair->aabb.getAsBoxMatrix());
				wi::renderer::DrawBox(hoverBox, XMFLOAT4(0, 0.5f, 0, 0.5f));
			}
		}

		// Spring visualizer:
		if (componentsWnd.springWnd.debugCheckBox.GetCheck())
		{
			for (size_t i = 0; i < scene.springs.GetCount(); ++i)
			{
				const SpringComponent& spring = scene.springs[i];
				wi::renderer::RenderablePoint point;
				point.position = spring.center_of_mass;
				point.size = 0.05f;
				point.color = XMFLOAT4(1, 1, 0, 1);
				wi::renderer::DrawPoint(point);
			}
		}
	}

	// Selected items box:
	if (!optionsWnd.cinemaModeCheckBox.GetCheck() && !translator.selected.empty())
	{
		AABB selectedAABB = AABB(
			XMFLOAT3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
			XMFLOAT3(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()));
		for (auto& picked : translator.selected)
		{
			if (picked.entity != INVALID_ENTITY)
			{
				const ObjectComponent* object = scene.objects.GetComponent(picked.entity);
				if (object != nullptr)
				{
					const AABB& aabb = *scene.aabb_objects.GetComponent(picked.entity);
					selectedAABB = AABB::Merge(selectedAABB, aabb);
				}

				const LightComponent* light = scene.lights.GetComponent(picked.entity);
				if (light != nullptr)
				{
					const AABB& aabb = *scene.aabb_lights.GetComponent(picked.entity);
					selectedAABB = AABB::Merge(selectedAABB, aabb);
				}

				const DecalComponent* decal = scene.decals.GetComponent(picked.entity);
				if (decal != nullptr)
				{
					const AABB& aabb = *scene.aabb_decals.GetComponent(picked.entity);
					selectedAABB = AABB::Merge(selectedAABB, aabb);

					// also display decal OBB:
					XMFLOAT4X4 selectionBox;
					selectionBox = decal->world;
					wi::renderer::DrawBox(selectionBox, XMFLOAT4(1, 0, 1, 1));
				}

				const EnvironmentProbeComponent* probe = scene.probes.GetComponent(picked.entity);
				if (probe != nullptr)
				{
					const AABB& aabb = *scene.aabb_probes.GetComponent(picked.entity);
					selectedAABB = AABB::Merge(selectedAABB, aabb);
				}

				const wi::HairParticleSystem* hair = scene.hairs.GetComponent(picked.entity);
				if (hair != nullptr)
				{
					selectedAABB = AABB::Merge(selectedAABB, hair->aabb);
				}

			}
		}

		XMFLOAT4X4 selectionBox;
		XMStoreFloat4x4(&selectionBox, selectedAABB.getAsBoxMatrix());
		wi::renderer::DrawBox(selectionBox, XMFLOAT4(1, 1, 1, 1));
	}

	renderPath->Render();

	// Editor custom render:
	if (!optionsWnd.cinemaModeCheckBox.GetCheck())
	{
		GraphicsDevice* device = wi::graphics::GetDevice();
		CommandList cmd = device->BeginCommandList();
		device->EventBegin("Editor", cmd);

		// Selection outline:
		if (renderPath->GetDepthStencil() != nullptr && !translator.selected.empty())
		{
			device->EventBegin("Selection Outline Mask", cmd);

			Viewport vp;
			vp.width = (float)rt_selectionOutline[0].GetDesc().width;
			vp.height = (float)rt_selectionOutline[0].GetDesc().height;
			device->BindViewports(1, &vp, cmd);

			wi::image::Params fx;
			fx.enableFullScreen();
			fx.stencilComp = wi::image::STENCILMODE::STENCILMODE_EQUAL;

			// We will specify the stencil ref in user-space, don't care about engine stencil refs here:
			//	Otherwise would need to take into account engine ref and draw multiple permutations of stencil refs.
			fx.stencilRefMode = wi::image::STENCILREFMODE_USER;

			// Materials outline:
			{
				device->RenderPassBegin(&renderpass_selectionOutline[0], cmd);

				// Draw solid blocks of selected materials
				fx.stencilRef = EDITORSTENCILREF_HIGHLIGHT_MATERIAL;
				wi::image::Draw(wi::texturehelper::getWhite(), fx, cmd);

				device->RenderPassEnd(cmd);
			}

			// Objects outline:
			{
				device->RenderPassBegin(&renderpass_selectionOutline[1], cmd);

				// Draw solid blocks of selected objects
				fx.stencilRef = EDITORSTENCILREF_HIGHLIGHT_OBJECT;
				wi::image::Draw(wi::texturehelper::getWhite(), fx, cmd);

				device->RenderPassEnd(cmd);
			}

			device->EventEnd(cmd);
		}

		// Full resolution:
		{
			device->RenderPassBegin(&renderpass_editor, cmd);

			Viewport vp;
			vp.width = (float)editor_depthbuffer.GetDesc().width;
			vp.height = (float)editor_depthbuffer.GetDesc().height;
			device->BindViewports(1, &vp, cmd);

			// Selection color:
			float selectionColorIntensity = std::sin(selectionOutlineTimer * XM_2PI * 0.8f) * 0.5f + 0.5f;
			XMFLOAT4 glow = wi::math::Lerp(wi::math::Lerp(XMFLOAT4(1, 1, 1, 1), selectionColor, 0.4f), selectionColor, selectionColorIntensity);
			wi::Color selectedEntityColor = wi::Color::fromFloat4(glow);

			// Draw selection outline to the screen:
			if (renderPath->GetDepthStencil() != nullptr && !translator.selected.empty())
			{
				device->EventBegin("Selection Outline Edge", cmd);
				wi::renderer::BindCommonResources(cmd);
				float opacity = wi::math::Lerp(0.4f, 1.0f, selectionColorIntensity);
				XMFLOAT4 col = selectionColor2;
				col.w *= opacity;
				wi::renderer::Postprocess_Outline(rt_selectionOutline[0], cmd, 0.1f, 1, col);
				col = selectionColor;
				col.w *= opacity;
				wi::renderer::Postprocess_Outline(rt_selectionOutline[1], cmd, 0.1f, 1, col);
				device->EventEnd(cmd);
			}

			const CameraComponent& camera = GetCurrentEditorScene().camera;

			const Scene& scene = GetCurrentScene();

			// remove camera jittering
			CameraComponent cam = *renderPath->camera;
			cam.jitter = XMFLOAT2(0, 0);
			cam.UpdateCamera();
			const XMMATRIX VP = cam.GetViewProjection();

			const XMMATRIX R = XMLoadFloat3x3(&cam.rotationMatrix);

			wi::font::Params fp;
			fp.customRotation = &R;
			fp.customProjection = &VP;
			fp.size = 32; // icon font render quality
			const float scaling = 0.0025f;
			fp.h_align = wi::font::WIFALIGN_CENTER;
			fp.v_align = wi::font::WIFALIGN_CENTER;
			fp.shadowColor = wi::Color::Shadow();
			fp.shadow_softness = 1;

			if (optionsWnd.rendererWnd.GetPickType() & PICK_LIGHT)
			{
				for (size_t i = 0; i < scene.lights.GetCount(); ++i)
				{
					const LightComponent& light = scene.lights[i];
					Entity entity = scene.lights.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}

					switch (light.GetType())
					{
					case LightComponent::POINT:
						wi::font::Draw(ICON_POINTLIGHT, fp, cmd);
						break;
					case LightComponent::SPOT:
						wi::font::Draw(ICON_SPOTLIGHT, fp, cmd);
						break;
					case LightComponent::DIRECTIONAL:
						wi::font::Draw(ICON_DIRECTIONALLIGHT, fp, cmd);
						break;
					default:
						break;
					}
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_DECAL)
			{
				for (size_t i = 0; i < scene.decals.GetCount(); ++i)
				{
					Entity entity = scene.decals.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_DECAL, fp, cmd);

				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_FORCEFIELD)
			{
				for (size_t i = 0; i < scene.forces.GetCount(); ++i)
				{
					Entity entity = scene.forces.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_FORCE, fp, cmd);
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_CAMERA)
			{
				for (size_t i = 0; i < scene.cameras.GetCount(); ++i)
				{
					Entity entity = scene.cameras.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_CAMERA, fp, cmd);
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_ARMATURE)
			{
				for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
				{
					Entity entity = scene.armatures.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_ARMATURE, fp, cmd);
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_EMITTER)
			{
				for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
				{
					Entity entity = scene.emitters.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_EMITTER, fp, cmd);
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_HAIR)
			{
				for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
				{
					Entity entity = scene.hairs.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_HAIR, fp, cmd);
				}
			}

			if (optionsWnd.rendererWnd.GetPickType() & PICK_SOUND)
			{
				for (size_t i = 0; i < scene.sounds.GetCount(); ++i)
				{
					Entity entity = scene.sounds.GetEntity(i);
					if (!scene.transforms.Contains(entity))
						continue;
					const TransformComponent& transform = *scene.transforms.GetComponent(entity);

					fp.position = transform.GetPosition();
					fp.scaling = scaling * wi::math::Distance(transform.GetPosition(), camera.Eye);
					fp.color = inactiveEntityColor;

					if (hovered.entity == entity)
					{
						fp.color = hoveredEntityColor;
					}
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							fp.color = selectedEntityColor;
							break;
						}
					}


					wi::font::Draw(ICON_SOUND, fp, cmd);
				}
			}
			if (bone_picking)
			{
				static PipelineState pso;
				if (!pso.IsValid())
				{
					static auto LoadShaders = [] {
						PipelineStateDesc desc;
						desc.vs = wi::renderer::GetShader(wi::enums::VSTYPE_VERTEXCOLOR);
						desc.ps = wi::renderer::GetShader(wi::enums::PSTYPE_VERTEXCOLOR);
						desc.il = wi::renderer::GetInputLayout(wi::enums::ILTYPE_VERTEXCOLOR);
						desc.dss = wi::renderer::GetDepthStencilState(wi::enums::DSSTYPE_DEPTHDISABLED);
						desc.rs = wi::renderer::GetRasterizerState(wi::enums::RSTYPE_DOUBLESIDED);
						desc.bs = wi::renderer::GetBlendState(wi::enums::BSTYPE_TRANSPARENT);
						desc.pt = PrimitiveTopology::TRIANGLELIST;
						wi::graphics::GetDevice()->CreatePipelineState(&desc, &pso);
					};
					static wi::eventhandler::Handle handle = wi::eventhandler::Subscribe(wi::eventhandler::EVENT_RELOAD_SHADERS, [](uint64_t userdata) { LoadShaders(); });
					LoadShaders();
				}

				size_t bone_count = 0;
				for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
				{
					const ArmatureComponent& armature = scene.armatures[i];
					bone_count += armature.boneCollection.size();
				}

				if (bone_count > 0)
				{
					struct Vertex
					{
						XMFLOAT4 position;
						XMFLOAT4 color;
					};
					const size_t segment_count = 18 + 1 + 18 + 1;
					const size_t vb_size = sizeof(Vertex) * (bone_count * (segment_count + 1 + 1));
					const size_t ib_size = sizeof(uint32_t) * bone_count * (segment_count + 1) * 3;
					GraphicsDevice::GPUAllocation mem = device->AllocateGPU(vb_size + ib_size, cmd);
					Vertex* vertices = (Vertex*)mem.data;
					uint32_t* indices = (uint32_t*)((uint8_t*)mem.data + vb_size);
					uint32_t vertex_count = 0;
					uint32_t index_count = 0;

					const XMVECTOR Eye = camera.GetEye();
					const XMVECTOR Unit = XMVectorSet(0, 1, 0, 0);

					for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
					{
						const ArmatureComponent& armature = scene.armatures[i];
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
							XMVECTOR ab = XMVector3Normalize(b - a);

							wi::primitive::Capsule capsule;
							capsule.radius = wi::math::Distance(a, b) * 0.1f;
							capsule.radius = wi::math::Clamp(capsule.radius, 0.01f, 0.1f);
							a -= ab * capsule.radius;
							b += ab * capsule.radius;
							XMStoreFloat3(&capsule.base, a);
							XMStoreFloat3(&capsule.tip, b);
							XMFLOAT4 color = inactiveEntityColor;

							if (scene.springs.Contains(entity))
							{
								color = wi::Color(255, 70, 165, uint8_t(color.w * 255));
							}
							if (scene.inverse_kinematics.Contains(entity))
							{
								color = wi::Color(49, 190, 103, uint8_t(color.w * 255));
							}

							if (hovered.entity == entity)
							{
								color = hoveredEntityColor;
							}
							for (auto& picked : translator.selected)
							{
								if (picked.entity == entity)
								{
									color = selectedEntityColor;
									break;
								}
							}

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

							uint32_t center_vertex_index = vertex_count;
							Vertex center_vertex;
							XMStoreFloat4(&center_vertex.position, A);
							center_vertex.position.w = 1;
							center_vertex.color = color;
							center_vertex.color.w = 0;
							std::memcpy(vertices + vertex_count, &center_vertex, sizeof(center_vertex));
							vertex_count++;

							for (size_t i = 0; i < segment_count; ++i)
							{
								XMVECTOR segment_pos;
								const float angle0 = XM_PIDIV2 + (float)i / (float)segment_count * XM_2PI;
								if (i < 18)
								{
									segment_pos = XMVectorSet(sinf(angle0) * capsule.radius, cosf(angle0) * capsule.radius, 0, 1);
								}
								else if (i == 18)
								{
									segment_pos = XMVectorSet(sinf(angle0) * capsule.radius, cosf(angle0) * capsule.radius, 0, 1);
								}
								else if (i > 18 && i < 18 + 1 + 18)
								{
									segment_pos = AB + XMVectorSet(sinf(angle0) * capsule.radius * 0.5f, cosf(angle0) * capsule.radius * 0.5f, 0, 1);
								}
								else
								{
									segment_pos = AB + XMVectorSet(sinf(angle0) * capsule.radius * 0.5f, cosf(angle0) * capsule.radius * 0.5f, 0, 1);
								}
								segment_pos = XMVector3Transform(segment_pos, M);

								Vertex vertex;
								XMStoreFloat4(&vertex.position, segment_pos);
								vertex.position.w = 1;
								vertex.color = color;
								//vertex.color.w = 0;
								std::memcpy(vertices + vertex_count, &vertex, sizeof(vertex));
								uint32_t ind[] = { center_vertex_index,vertex_count - 1,vertex_count };
								std::memcpy(indices + index_count, ind, sizeof(ind));
								index_count += arraysize(ind);
								vertex_count++;
							}
							// closing triangle fan:
							uint32_t ind[] = { center_vertex_index,vertex_count - 1,center_vertex_index+1 };
							std::memcpy(indices + index_count, ind, sizeof(ind));
							index_count += arraysize(ind);
						}
					}

					device->EventBegin("Bone capsules", cmd);
					device->BindPipelineState(&pso, cmd);

					MiscCB sb;
					XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
					sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
					device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

					const GPUBuffer* vbs[] = {
						&mem.buffer,
					};
					const uint32_t strides[] = {
						sizeof(Vertex)
					};
					const uint64_t offsets[] = {
						mem.offset,
					};
					device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);
					device->BindIndexBuffer(&mem.buffer, IndexBufferFormat::UINT32, mem.offset + vb_size, cmd);

					device->DrawIndexed(index_count, 0, 0, cmd);
					device->EventEnd(cmd);
				}
			}

			if (optionsWnd.rendererWnd.nameDebugCheckBox.GetCheck())
			{
				device->EventBegin("Debug Names", cmd);
				struct DebugNameEntitySorter
				{
					size_t name_index;
					float distance;
					XMFLOAT3 position;
				};
				static wi::vector<DebugNameEntitySorter> debugNameEntitiesSorted;
				debugNameEntitiesSorted.clear();
				for (size_t i = 0; i < scene.names.GetCount(); ++i)
				{
					Entity entity = scene.names.GetEntity(i);
					const TransformComponent* transform = scene.transforms.GetComponent(entity);
					if (transform != nullptr)
					{
						auto& x = debugNameEntitiesSorted.emplace_back();
						x.name_index = i;
						x.position = transform->GetPosition();
						const ObjectComponent* object = scene.objects.GetComponent(entity);
						if (object != nullptr)
						{
							x.position = object->center;
						}
						x.distance = wi::math::Distance(x.position, camera.Eye);
					}
				}
				std::sort(debugNameEntitiesSorted.begin(), debugNameEntitiesSorted.end(), [](const DebugNameEntitySorter& a, const DebugNameEntitySorter& b)
					{
						return a.distance > b.distance;
					});
				for (auto& x : debugNameEntitiesSorted)
				{
					Entity entity = scene.names.GetEntity(x.name_index);
					wi::font::Params params;
					params.position = x.position;
					params.size = wi::font::WIFONTSIZE_DEFAULT;
					params.scaling = 1.0f / params.size * x.distance * 0.03f;
					params.color = wi::Color::White();
					for (auto& picked : translator.selected)
					{
						if (picked.entity == entity)
						{
							params.color = selectedEntityColor;
							break;
						}
					}
					params.h_align = wi::font::WIFALIGN_CENTER;
					params.v_align = wi::font::WIFALIGN_CENTER;
					params.softness = 0.1f;
					params.shadowColor = wi::Color::Black();
					params.shadow_softness = 0.5f;
					params.customProjection = &VP;
					params.customRotation = &R;
					wi::font::Draw(scene.names[x.name_index].name, params, cmd);
				}
				device->EventEnd(cmd);
			}

			if (inspector_mode)
			{
				std::string str;
				str += "Entity: " + std::to_string(hovered.entity);
				const NameComponent* name = scene.names.GetComponent(hovered.entity);
				if (name != nullptr)
				{
					str += "\nName: " + name->name;
				}
				XMFLOAT4 pointer = wi::input::GetPointer();
				wi::font::Params params;
				params.position = XMFLOAT3(pointer.x - 10, pointer.y, 0);
				params.shadowColor = wi::Color::Black();
				params.h_align = wi::font::WIFALIGN_RIGHT;
				params.v_align = wi::font::WIFALIGN_CENTER;
				wi::font::Draw(str, params, cmd);
			}


			optionsWnd.paintToolWnd.DrawBrush(*this, cmd);
			if (optionsWnd.paintToolWnd.GetMode() == PaintToolWindow::MODE::MODE_DISABLED)
			{
				translator.Draw(GetCurrentEditorScene().camera, cmd);
			}

			device->RenderPassEnd(cmd);
		}

		device->EventEnd(cmd);
	}

	RenderPath2D::Render();

}
void EditorComponent::Compose(CommandList cmd) const
{
	renderPath->Compose(cmd);

	RenderPath2D::Compose(cmd);
}

void EditorComponent::ClearSelected()
{
	translator.selected.clear();
}
void EditorComponent::AddSelected(Entity entity)
{
	wi::scene::PickResult res;
	res.entity = entity;
	AddSelected(res);
}
void EditorComponent::AddSelected(const PickResult& picked)
{
	bool removal = false;
	for (size_t i = 0; i < translator.selected.size(); ++i)
	{
		if(translator.selected[i] == picked)
		{
			// If already selected, it will be deselected now:
			translator.selected[i] = translator.selected.back();
			translator.selected.pop_back();
			removal = true;
			break;
		}
	}

	if (!removal)
	{
		translator.selected.push_back(picked);
	}
}
bool EditorComponent::IsSelected(Entity entity) const
{
	for (auto& x : translator.selected)
	{
		if (x.entity == entity)
		{
			return true;
		}
	}
	return false;
}

void EditorComponent::RecordSelection(wi::Archive& archive) const
{
	archive << translator.selected.size();
	for (auto& x : translator.selected)
	{
		archive << x.entity;
		archive << x.position;
		archive << x.normal;
		archive << x.subsetIndex;
		archive << x.distance;
	}
}
void EditorComponent::RecordEntity(wi::Archive& archive, wi::ecs::Entity entity)
{
	const wi::vector<Entity> entities = { entity };
	RecordEntity(archive, entities);
}
void EditorComponent::RecordEntity(wi::Archive& archive, const wi::vector<wi::ecs::Entity>& entities)
{
	Scene& scene = GetCurrentScene();
	EntitySerializer seri;

	archive << entities;
	for (auto& x : entities)
	{
		scene.Entity_Serialize(archive, seri, x);
	}
}

void EditorComponent::ResetHistory()
{
	EditorScene& editorscene = GetCurrentEditorScene();
	editorscene.historyPos = -1;
	editorscene.history.clear();
}
wi::Archive& EditorComponent::AdvanceHistory()
{
	EditorScene& editorscene = GetCurrentEditorScene();
	editorscene.historyPos++;

	while (static_cast<int>(editorscene.history.size()) > editorscene.historyPos)
	{
		editorscene.history.pop_back();
	}

	editorscene.history.emplace_back();
	editorscene.history.back().SetReadModeAndResetPos(false);

	return editorscene.history.back();
}
void EditorComponent::ConsumeHistoryOperation(bool undo)
{
	EditorScene& editorscene = GetCurrentEditorScene();

	if ((undo && editorscene.historyPos >= 0) || (!undo && editorscene.historyPos < (int)editorscene.history.size() - 1))
	{
		if (!undo)
		{
			editorscene.historyPos++;
		}

		Scene& scene = GetCurrentScene();

		wi::Archive& archive = editorscene.history[editorscene.historyPos];
		archive.SetReadModeAndResetPos(true);

		int temp;
		archive >> temp;
		HistoryOperationType type = (HistoryOperationType)temp;

		switch (type)
		{
		case HISTORYOP_TRANSLATOR:
			{
				archive >> translator.isTranslator;
				archive >> translator.isRotator;
				archive >> translator.isScalator;
				optionsWnd.isTranslatorCheckBox.SetCheck(translator.isTranslator);
				optionsWnd.isRotatorCheckBox.SetCheck(translator.isRotator);
				optionsWnd.isScalatorCheckBox.SetCheck(translator.isScalator);

				EntitySerializer seri;
				wi::scene::TransformComponent start;
				wi::scene::TransformComponent end;
				start.Serialize(archive, seri);
				end.Serialize(archive, seri);
				wi::vector<XMFLOAT4X4> matrices_start;
				wi::vector<XMFLOAT4X4> matrices_end;
				archive >> matrices_start;
				archive >> matrices_end;

				translator.PreTranslate();
				if (undo)
				{
					translator.transform = start;
					translator.matrices_current = matrices_start;
				}
				else
				{
					translator.transform = end;
					translator.matrices_current = matrices_end;
				}
				translator.transform.UpdateTransform();
				translator.PostTranslate();
			}
			break;
		case HISTORYOP_SELECTION:
		{
			// Read selections states from archive:

			wi::vector<wi::scene::PickResult> selectedBEFORE;
			size_t selectionCountBEFORE;
			archive >> selectionCountBEFORE;
			for (size_t i = 0; i < selectionCountBEFORE; ++i)
			{
				wi::scene::PickResult sel;
				archive >> sel.entity;
				archive >> sel.position;
				archive >> sel.normal;
				archive >> sel.subsetIndex;
				archive >> sel.distance;

				selectedBEFORE.push_back(sel);
			}

			wi::vector<wi::scene::PickResult> selectedAFTER;
			size_t selectionCountAFTER;
			archive >> selectionCountAFTER;
			for (size_t i = 0; i < selectionCountAFTER; ++i)
			{
				wi::scene::PickResult sel;
				archive >> sel.entity;
				archive >> sel.position;
				archive >> sel.normal;
				archive >> sel.subsetIndex;
				archive >> sel.distance;

				selectedAFTER.push_back(sel);
			}

			// Restore proper selection state:
			if (undo)
			{
				translator.selected = selectedBEFORE;
			}
			else
			{
				translator.selected = selectedAFTER;
			}
		}
		break;
		case HISTORYOP_ADD:
		{
			// Read selections states from archive:

			wi::vector<wi::scene::PickResult> selectedBEFORE;
			size_t selectionCountBEFORE;
			archive >> selectionCountBEFORE;
			for (size_t i = 0; i < selectionCountBEFORE; ++i)
			{
				wi::scene::PickResult sel;
				archive >> sel.entity;
				archive >> sel.position;
				archive >> sel.normal;
				archive >> sel.subsetIndex;
				archive >> sel.distance;

				selectedBEFORE.push_back(sel);
			}

			wi::vector<wi::scene::PickResult> selectedAFTER;
			size_t selectionCountAFTER;
			archive >> selectionCountAFTER;
			for (size_t i = 0; i < selectionCountAFTER; ++i)
			{
				wi::scene::PickResult sel;
				archive >> sel.entity;
				archive >> sel.position;
				archive >> sel.normal;
				archive >> sel.subsetIndex;
				archive >> sel.distance;

				selectedAFTER.push_back(sel);
			}

			wi::vector<Entity> addedEntities;
			archive >> addedEntities;

			ClearSelected();
			if (undo)
			{
				translator.selected = selectedBEFORE;
				for (size_t i = 0; i < addedEntities.size(); ++i)
				{
					scene.Entity_Remove(addedEntities[i]);
				}
			}
			else
			{
				translator.selected = selectedAFTER;
				EntitySerializer seri;
				seri.allow_remap = false;
				for (size_t i = 0; i < addedEntities.size(); ++i)
				{
					scene.Entity_Serialize(archive, seri);
				}
			}

		}
		break;
		case HISTORYOP_DELETE:
			{
				// Read selections states from archive:

				wi::vector<wi::scene::PickResult> selectedBEFORE;
				size_t selectionCountBEFORE;
				archive >> selectionCountBEFORE;
				for (size_t i = 0; i < selectionCountBEFORE; ++i)
				{
					wi::scene::PickResult sel;
					archive >> sel.entity;
					archive >> sel.position;
					archive >> sel.normal;
					archive >> sel.subsetIndex;
					archive >> sel.distance;

					selectedBEFORE.push_back(sel);
				}

				wi::vector<Entity> deletedEntities;
				archive >> deletedEntities;

				ClearSelected();
				if (undo)
				{
					translator.selected = selectedBEFORE;
					EntitySerializer seri;
					seri.allow_remap = false;
					for (size_t i = 0; i < deletedEntities.size(); ++i)
					{
						scene.Entity_Serialize(archive, seri);
					}
				}
				else
				{
					for (size_t i = 0; i < deletedEntities.size(); ++i)
					{
						scene.Entity_Remove(deletedEntities[i]);
					}
				}

			}
			break;
		case HISTORYOP_COMPONENT_DATA:
			{
				Scene before, after;
				wi::vector<Entity> entities_before, entities_after;

				archive >> entities_before;
				for (auto& x : entities_before)
				{
					EntitySerializer seri;
					seri.allow_remap = false;
					before.Entity_Serialize(archive, seri);
				}

				archive >> entities_after;
				for (auto& x : entities_after)
				{
					EntitySerializer seri;
					seri.allow_remap = false;
					after.Entity_Serialize(archive, seri);
				}

				if (undo)
				{
					for (auto& x : entities_before)
					{
						scene.Entity_Remove(x);
					}
					scene.Merge(before);
				}
				else
				{
					for (auto& x : entities_after)
					{
						scene.Entity_Remove(x);
					}
					scene.Merge(after);
				}
			}
			break;
		case HISTORYOP_PAINTTOOL:
			optionsWnd.paintToolWnd.ConsumeHistoryOperation(archive, undo);
			break;
		case HISTORYOP_NONE:
		default:
			assert(0);
			break;
		}

		if (undo)
		{
			editorscene.historyPos--;
		}

		scene.Update(0);
	}

	optionsWnd.RefreshEntityTree();
}

void EditorComponent::Save(const std::string& filename)
{
	const bool dump_to_header = optionsWnd.saveModeComboBox.GetSelected() == 2;

	wi::Archive archive = dump_to_header ? wi::Archive() : wi::Archive(filename, false);
	if (archive.IsOpen())
	{
		Scene& scene = GetCurrentScene();

		wi::resourcemanager::Mode embed_mode = (wi::resourcemanager::Mode)optionsWnd.saveModeComboBox.GetItemUserData(optionsWnd.saveModeComboBox.GetSelected());
		wi::resourcemanager::SetMode(embed_mode);

		optionsWnd.terragen.BakeVirtualTexturesToFiles();
		scene.Serialize(archive);

		if (dump_to_header)
		{
			archive.SaveHeaderFile(filename, wi::helper::RemoveExtension(wi::helper::GetFileNameFromPath(filename)));
		}

		GetCurrentEditorScene().path = filename;
	}
	else
	{
		wi::helper::messageBox("Could not create " + filename + "!");
		return;
	}

	RefreshSceneList();

	wi::backlog::post("Scene " + std::to_string(current_scene) + " saved: " + GetCurrentEditorScene().path);
}
void EditorComponent::SaveAs()
{
	const bool dump_to_header = optionsWnd.saveModeComboBox.GetSelected() == 2;

	wi::helper::FileDialogParams params;
	params.type = wi::helper::FileDialogParams::SAVE;
	if (dump_to_header)
	{
		params.description = "C++ header (.h)";
		params.extensions.push_back("h");
	}
	else
	{
		params.description = "Wicked Scene (.wiscene)";
		params.extensions.push_back("wiscene");
	}
	wi::helper::FileDialog(params, [=](std::string fileName) {
		wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
			std::string filename = wi::helper::ReplaceExtension(fileName, params.extensions.front());
			Save(filename);
			});
		});
}

void EditorComponent::CheckBonePickingEnabled()
{
	// Check if armature or bone is selected to allow bone picking:
	//	(Don't want to always enable bone picking, because it can make the screen look very busy.)
	Scene& scene = GetCurrentScene();

	bone_picking = false;
	for (size_t i = 0; i < scene.armatures.GetCount() && !bone_picking; ++i)
	{
		Entity entity = scene.armatures.GetEntity(i);
		for (auto& x : translator.selected)
		{
			if (entity == x.entity)
			{
				bone_picking = true;
				break;
			}
		}
		for (Entity bone : scene.armatures[i].boneCollection)
		{
			for (auto& x : translator.selected)
			{
				if (bone == x.entity)
				{
					bone_picking = true;
					break;
				}
			}
		}
	}
}

