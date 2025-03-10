#include "stdafx.h"
#include "MeshWindow.h"
#include "Editor.h"

#include "Utility/stb_image.h"

#include "meshoptimizer/meshoptimizer.h"

#include <string>

using namespace wi::ecs;
using namespace wi::scene;

void MeshWindow::Create(EditorComponent* _editor)
{
	editor = _editor;
	wi::gui::Window::Create(ICON_MESH " Mesh", wi::gui::Window::WindowControls::COLLAPSE);
	SetSize(XMFLOAT2(580, 720));

	float x = 95;
	float y = 0;
	float hei = 18;
	float step = hei + 2;
	float wid = 170;

	float infolabel_height = 190;
	meshInfoLabel.Create("Mesh Info");
	meshInfoLabel.SetPos(XMFLOAT2(20, y));
	meshInfoLabel.SetSize(XMFLOAT2(260, infolabel_height));
	meshInfoLabel.SetColor(wi::Color::Transparent());
	AddWidget(&meshInfoLabel);

	// Left side:
	y = infolabel_height + 5;

	subsetComboBox.Create("Select subset: ");
	subsetComboBox.SetSize(XMFLOAT2(40, hei));
	subsetComboBox.SetPos(XMFLOAT2(x, y));
	subsetComboBox.SetEnabled(false);
	subsetComboBox.OnSelect([=](wi::gui::EventArgs args) {
		Scene& scene = editor->GetCurrentScene();
		MeshComponent* mesh = scene.meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			subset = args.iValue;
			if (!editor->translator.selected.empty())
			{
				editor->translator.selected.back().subsetIndex = subset;
			}
		}
	});
	subsetComboBox.SetTooltip("Select a subset. A subset can also be selected by picking it in the 3D scene.\nLook at the material window when a subset is selected to edit it.");
	AddWidget(&subsetComboBox);

	doubleSidedCheckBox.Create("Double Sided: ");
	doubleSidedCheckBox.SetTooltip("If enabled, the inside of the mesh will be visible.");
	doubleSidedCheckBox.SetSize(XMFLOAT2(hei, hei));
	doubleSidedCheckBox.SetPos(XMFLOAT2(x, y += step));
	doubleSidedCheckBox.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->SetDoubleSided(args.bValue);
		}
	});
	AddWidget(&doubleSidedCheckBox);

	softbodyCheckBox.Create("Soft body: ");
	softbodyCheckBox.SetTooltip("Enable soft body simulation. Tip: Use the Paint Tool to control vertex pinning.");
	softbodyCheckBox.SetSize(XMFLOAT2(hei, hei));
	softbodyCheckBox.SetPos(XMFLOAT2(x, y += step));
	softbodyCheckBox.OnClick([&](wi::gui::EventArgs args) {

		Scene& scene = editor->GetCurrentScene();
		SoftBodyPhysicsComponent* physicscomponent = scene.softbodies.GetComponent(entity);

		if (args.bValue)
		{
			if (physicscomponent == nullptr)
			{
				SoftBodyPhysicsComponent& softbody = scene.softbodies.Create(entity);
				softbody.friction = frictionSlider.GetValue();
				softbody.restitution = restitutionSlider.GetValue();
				softbody.mass = massSlider.GetValue();
			}
		}
		else
		{
			if (physicscomponent != nullptr)
			{
				scene.softbodies.Remove(entity);
				MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
				if (mesh != nullptr)
				{
					mesh->CreateRenderData();
				}
			}
		}

	});
	AddWidget(&softbodyCheckBox);

	massSlider.Create(0, 10, 1, 100000, "Mass: ");
	massSlider.SetTooltip("Set the mass amount for the physics engine.");
	massSlider.SetSize(XMFLOAT2(wid, hei));
	massSlider.SetPos(XMFLOAT2(x, y += step));
	massSlider.OnSlide([&](wi::gui::EventArgs args) {
		SoftBodyPhysicsComponent* physicscomponent = editor->GetCurrentScene().softbodies.GetComponent(entity);
		if (physicscomponent != nullptr)
		{
			physicscomponent->mass = args.fValue;
		}
	});
	AddWidget(&massSlider);

	frictionSlider.Create(0, 1, 0.5f, 100000, "Friction: ");
	frictionSlider.SetTooltip("Set the friction amount for the physics engine.");
	frictionSlider.SetSize(XMFLOAT2(wid, hei));
	frictionSlider.SetPos(XMFLOAT2(x, y += step));
	frictionSlider.OnSlide([&](wi::gui::EventArgs args) {
		SoftBodyPhysicsComponent* physicscomponent = editor->GetCurrentScene().softbodies.GetComponent(entity);
		if (physicscomponent != nullptr)
		{
			physicscomponent->friction = args.fValue;
		}
	});
	AddWidget(&frictionSlider);

	restitutionSlider.Create(0, 1, 0, 100000, "Restitution: ");
	restitutionSlider.SetTooltip("Set the restitution amount for the physics engine.");
	restitutionSlider.SetSize(XMFLOAT2(wid, hei));
	restitutionSlider.SetPos(XMFLOAT2(x, y += step));
	restitutionSlider.OnSlide([&](wi::gui::EventArgs args) {
		SoftBodyPhysicsComponent* physicscomponent = editor->GetCurrentScene().softbodies.GetComponent(entity);
		if (physicscomponent != nullptr)
		{
			physicscomponent->restitution = args.fValue;
		}
		});
	AddWidget(&restitutionSlider);

	impostorCreateButton.Create("Create Impostor");
	impostorCreateButton.SetTooltip("Create an impostor image of the mesh. The mesh will be replaced by this image when far away, to render faster.");
	impostorCreateButton.SetSize(XMFLOAT2(wid, hei));
	impostorCreateButton.SetPos(XMFLOAT2(x, y += step));
	impostorCreateButton.OnClick([&](wi::gui::EventArgs args) {
		Scene& scene = editor->GetCurrentScene();
		ImpostorComponent* impostor = scene.impostors.GetComponent(entity);
		if (impostor == nullptr)
		{
			impostorCreateButton.SetText("Delete Impostor");
			scene.impostors.Create(entity).swapInDistance = impostorDistanceSlider.GetValue();
		}
		else
		{
			impostorCreateButton.SetText("Create Impostor");
			scene.impostors.Remove(entity);
		}
	});
	AddWidget(&impostorCreateButton);

	impostorDistanceSlider.Create(0, 1000, 100, 10000, "Impostor Dist: ");
	impostorDistanceSlider.SetTooltip("Assign the distance where the mesh geometry should be switched to the impostor image.");
	impostorDistanceSlider.SetSize(XMFLOAT2(wid, hei));
	impostorDistanceSlider.SetPos(XMFLOAT2(x, y += step));
	impostorDistanceSlider.OnSlide([&](wi::gui::EventArgs args) {
		ImpostorComponent* impostor = editor->GetCurrentScene().impostors.GetComponent(entity);
		if (impostor != nullptr)
		{
			impostor->swapInDistance = args.fValue;
		}
	});
	AddWidget(&impostorDistanceSlider);

	tessellationFactorSlider.Create(0, 100, 0, 10000, "Tess Factor: ");
	tessellationFactorSlider.SetTooltip("Set the dynamic tessellation amount. Tessellation should be enabled in the Renderer window and your GPU must support it!");
	tessellationFactorSlider.SetSize(XMFLOAT2(wid, hei));
	tessellationFactorSlider.SetPos(XMFLOAT2(x, y += step));
	tessellationFactorSlider.OnSlide([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->tessellationFactor = args.fValue;
		}
	});
	AddWidget(&tessellationFactorSlider);

	float mod_x = x - 20;
	float mod_wid = wid + 40;

	flipCullingButton.Create("Flip Culling");
	flipCullingButton.SetTooltip("Flip faces to reverse triangle culling order.");
	flipCullingButton.SetSize(XMFLOAT2(mod_wid, hei));
	flipCullingButton.SetPos(XMFLOAT2(mod_x, y += step));
	flipCullingButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->FlipCulling();
			SetEntity(entity, subset);
		}
	});
	AddWidget(&flipCullingButton);

	flipNormalsButton.Create("Flip Normals");
	flipNormalsButton.SetTooltip("Flip surface normals.");
	flipNormalsButton.SetSize(XMFLOAT2(mod_wid, hei));
	flipNormalsButton.SetPos(XMFLOAT2(mod_x, y += step));
	flipNormalsButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->FlipNormals();
			SetEntity(entity, subset);
		}
	});
	AddWidget(&flipNormalsButton);

	computeNormalsSmoothButton.Create("Compute Normals [SMOOTH]");
	computeNormalsSmoothButton.SetTooltip("Compute surface normals of the mesh. Resulting normals will be unique per vertex. This can reduce vertex count, but is slow.");
	computeNormalsSmoothButton.SetSize(XMFLOAT2(mod_wid, hei));
	computeNormalsSmoothButton.SetPos(XMFLOAT2(mod_x, y += step));
	computeNormalsSmoothButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->ComputeNormals(MeshComponent::COMPUTE_NORMALS_SMOOTH);
			SetEntity(entity, subset);
		}
	});
	AddWidget(&computeNormalsSmoothButton);

	computeNormalsHardButton.Create("Compute Normals [HARD]");
	computeNormalsHardButton.SetTooltip("Compute surface normals of the mesh. Resulting normals will be unique per face. This can increase vertex count.");
	computeNormalsHardButton.SetSize(XMFLOAT2(mod_wid, hei));
	computeNormalsHardButton.SetPos(XMFLOAT2(mod_x, y += step));
	computeNormalsHardButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->ComputeNormals(MeshComponent::COMPUTE_NORMALS_HARD);
			SetEntity(entity, subset);
		}
	});
	AddWidget(&computeNormalsHardButton);

	recenterButton.Create("Recenter");
	recenterButton.SetTooltip("Recenter mesh to AABB center.");
	recenterButton.SetSize(XMFLOAT2(mod_wid, hei));
	recenterButton.SetPos(XMFLOAT2(mod_x, y += step));
	recenterButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->Recenter();
			SetEntity(entity, subset);
		}
	});
	AddWidget(&recenterButton);

	recenterToBottomButton.Create("RecenterToBottom");
	recenterToBottomButton.SetTooltip("Recenter mesh to AABB bottom.");
	recenterToBottomButton.SetSize(XMFLOAT2(mod_wid, hei));
	recenterToBottomButton.SetPos(XMFLOAT2(mod_x, y += step));
	recenterToBottomButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			mesh->RecenterToBottom();
			SetEntity(entity, subset);
		}
	});
	AddWidget(&recenterToBottomButton);

	mergeButton.Create("Merge Selected");
	mergeButton.SetTooltip("Merges selected objects/meshes into one.");
	mergeButton.SetSize(XMFLOAT2(mod_wid, hei));
	mergeButton.SetPos(XMFLOAT2(mod_x, y += step));
	mergeButton.OnClick([=](wi::gui::EventArgs args) {
		Scene& scene = editor->GetCurrentScene();
		MeshComponent merged_mesh;
		bool valid_normals = false;
		bool valid_uvset_0 = false;
		bool valid_uvset_1 = false;
		bool valid_atlas = false;
		bool valid_boneindices = false;
		bool valid_boneweights = false;
		bool valid_colors = false;
		bool valid_windweights = false;
		wi::unordered_set<Entity> entities_to_remove;
		Entity prev_subset_material = INVALID_ENTITY;
		for (auto& picked : editor->translator.selected)
		{
			ObjectComponent* object = scene.objects.GetComponent(picked.entity);
			if (object == nullptr)
				continue;
			MeshComponent* mesh = scene.meshes.GetComponent(object->meshID);
			if (mesh == nullptr)
				continue;
			const TransformComponent* transform = scene.transforms.GetComponent(picked.entity);
			XMMATRIX W = XMLoadFloat4x4(&transform->world);
			uint32_t vertexOffset = (uint32_t)merged_mesh.vertex_positions.size();
			uint32_t indexOffset = (uint32_t)merged_mesh.indices.size();
			for (auto& ind : mesh->indices)
			{
				merged_mesh.indices.push_back(vertexOffset + ind);
			}
			uint32_t first_subset = 0;
			uint32_t last_subset = 0;
			mesh->GetLODSubsetRange(0, first_subset, last_subset);
			for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
			{
				const MeshComponent::MeshSubset& subset = mesh->subsets[subsetIndex];
				if (subset.materialID != prev_subset_material)
				{
					// new subset
					prev_subset_material = subset.materialID;
					merged_mesh.subsets.push_back(subset);
					merged_mesh.subsets.back().indexOffset += indexOffset;
				}
				else
				{
					// append to previous subset
					merged_mesh.subsets.back().indexCount += subset.indexCount;
				}
			}
			for (size_t i = 0; i < mesh->vertex_positions.size(); ++i)
			{
				merged_mesh.vertex_positions.push_back(mesh->vertex_positions[i]);
				XMStoreFloat3(&merged_mesh.vertex_positions.back(), XMVector3Transform(XMLoadFloat3(&merged_mesh.vertex_positions.back()), W));

				if (mesh->vertex_normals.empty())
				{
					merged_mesh.vertex_normals.emplace_back();
				}
				else
				{
					valid_normals = true;
					merged_mesh.vertex_normals.push_back(mesh->vertex_normals[i]);
					XMStoreFloat3(&merged_mesh.vertex_normals.back(), XMVector3TransformNormal(XMLoadFloat3(&merged_mesh.vertex_normals.back()), W));
				}

				if (mesh->vertex_uvset_0.empty())
				{
					merged_mesh.vertex_uvset_0.emplace_back();
				}
				else
				{
					valid_uvset_0 = true;
					merged_mesh.vertex_uvset_0.push_back(mesh->vertex_uvset_0[i]);
				}

				if (mesh->vertex_uvset_1.empty())
				{
					merged_mesh.vertex_uvset_1.emplace_back();
				}
				else
				{
					valid_uvset_1 = true;
					merged_mesh.vertex_uvset_1.push_back(mesh->vertex_uvset_1[i]);
				}

				if (mesh->vertex_atlas.empty())
				{
					merged_mesh.vertex_atlas.emplace_back();
				}
				else
				{
					valid_atlas = true;
					merged_mesh.vertex_atlas.push_back(mesh->vertex_atlas[i]);
				}

				if (mesh->vertex_boneindices.empty())
				{
					merged_mesh.vertex_boneindices.emplace_back();
				}
				else
				{
					valid_boneindices = true;
					merged_mesh.vertex_boneindices.push_back(mesh->vertex_boneindices[i]);
				}

				if (mesh->vertex_boneweights.empty())
				{
					merged_mesh.vertex_boneweights.emplace_back();
				}
				else
				{
					valid_boneweights = true;
					merged_mesh.vertex_boneweights.push_back(mesh->vertex_boneweights[i]);
				}

				if (mesh->vertex_colors.empty())
				{
					merged_mesh.vertex_colors.push_back(~0u);
				}
				else
				{
					valid_colors = true;
					merged_mesh.vertex_colors.push_back(mesh->vertex_colors[i]);
				}

				if (mesh->vertex_windweights.empty())
				{
					merged_mesh.vertex_windweights.emplace_back();
				}
				else
				{
					valid_windweights = true;
					merged_mesh.vertex_windweights.push_back(mesh->vertex_windweights[i]);
				}
			}
			if (merged_mesh.armatureID == INVALID_ENTITY)
			{
				merged_mesh.armatureID = mesh->armatureID;
			}
			entities_to_remove.insert(object->meshID);
			entities_to_remove.insert(picked.entity);
		}

		if (!merged_mesh.vertex_positions.empty())
		{
			if (!valid_normals)
				merged_mesh.vertex_normals.clear();
			if (!valid_uvset_0)
				merged_mesh.vertex_uvset_0.clear();
			if (!valid_uvset_1)
				merged_mesh.vertex_uvset_1.clear();
			if (!valid_atlas)
				merged_mesh.vertex_atlas.clear();
			if (!valid_boneindices)
				merged_mesh.vertex_boneindices.clear();
			if (!valid_boneweights)
				merged_mesh.vertex_boneweights.clear();
			if (!valid_colors)
				merged_mesh.vertex_colors.clear();
			if (!valid_windweights)
				merged_mesh.vertex_windweights.clear();

			Entity merged_object_entity = scene.Entity_CreateObject("mergedObject");
			Entity merged_mesh_entity = scene.Entity_CreateMesh("mergedMesh");
			ObjectComponent* object = scene.objects.GetComponent(merged_object_entity);
			object->meshID = merged_mesh_entity;
			MeshComponent* mesh = scene.meshes.GetComponent(merged_mesh_entity);
			*mesh = std::move(merged_mesh);
			mesh->CreateRenderData();
		}

		for (auto& x : entities_to_remove)
		{
			scene.Entity_Remove(x);
		}
		
	});
	AddWidget(&mergeButton);

	optimizeButton.Create("Optimize");
	optimizeButton.SetTooltip("Run the meshoptimizer library.");
	optimizeButton.SetSize(XMFLOAT2(mod_wid, hei));
	optimizeButton.SetPos(XMFLOAT2(mod_x, y += step));
	optimizeButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			// https://github.com/zeux/meshoptimizer#vertex-cache-optimization

			size_t index_count = mesh->indices.size();
			size_t vertex_count = mesh->vertex_positions.size();

			wi::vector<uint32_t> indices(index_count);
			meshopt_optimizeVertexCache(indices.data(), mesh->indices.data(), index_count, vertex_count);

			mesh->indices = indices;

			mesh->CreateRenderData();
			SetEntity(entity, subset);
		}
		});
	AddWidget(&optimizeButton);



	subsetMaterialComboBox.Create("Material: ");
	subsetMaterialComboBox.SetSize(XMFLOAT2(wid, hei));
	subsetMaterialComboBox.SetPos(XMFLOAT2(x, y += step));
	subsetMaterialComboBox.SetEnabled(false);
	subsetMaterialComboBox.OnSelect([&](wi::gui::EventArgs args) {
		Scene& scene = editor->GetCurrentScene();
		MeshComponent* mesh = scene.meshes.GetComponent(entity);
		if (mesh != nullptr && subset >= 0 && subset < mesh->subsets.size())
		{
			MeshComponent::MeshSubset& meshsubset = mesh->subsets[subset];
			if (args.iValue == 0)
			{
				meshsubset.materialID = INVALID_ENTITY;
			}
			else
			{
				MeshComponent::MeshSubset& meshsubset = mesh->subsets[subset];
				meshsubset.materialID = scene.materials.GetEntity(args.iValue - 1);
			}
		}
		});
	subsetMaterialComboBox.SetTooltip("Set the base material of the selected MeshSubset");
	AddWidget(&subsetMaterialComboBox);


	morphTargetCombo.Create("Morph Target:");
	morphTargetCombo.SetSize(XMFLOAT2(wid, hei));
	morphTargetCombo.SetPos(XMFLOAT2(x, y += step));
	morphTargetCombo.OnSelect([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr && args.iValue < (int)mesh->morph_targets.size())
		{
			morphTargetSlider.SetValue(mesh->morph_targets[args.iValue].weight);
		}
	});
	morphTargetCombo.SetTooltip("Choose a morph target to edit weight.");
	AddWidget(&morphTargetCombo);

	morphTargetSlider.Create(0, 1, 0, 100000, "Weight: ");
	morphTargetSlider.SetTooltip("Set the weight for morph target");
	morphTargetSlider.SetSize(XMFLOAT2(wid, hei));
	morphTargetSlider.SetPos(XMFLOAT2(x, y += step));
	morphTargetSlider.OnSlide([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr && morphTargetCombo.GetSelected() < (int)mesh->morph_targets.size())
		{
			mesh->morph_targets[morphTargetCombo.GetSelected()].weight = args.fValue;
			mesh->dirty_morph = true;
		}
	});
	AddWidget(&morphTargetSlider);

	lodgenButton.Create("LOD Gen");
	lodgenButton.SetTooltip("Generate LODs (levels of detail).");
	lodgenButton.SetSize(XMFLOAT2(wid, hei));
	lodgenButton.SetPos(XMFLOAT2(x, y += step));
	lodgenButton.OnClick([&](wi::gui::EventArgs args) {
		MeshComponent* mesh = editor->GetCurrentScene().meshes.GetComponent(entity);
		if (mesh != nullptr)
		{
			if (mesh->subsets_per_lod == 0)
			{
				// if there were no lods before, record the subset count without lods:
				mesh->subsets_per_lod = (uint32_t)mesh->subsets.size();
			}

			// https://github.com/zeux/meshoptimizer/blob/bedaaaf6e710d3b42d49260ca738c15d171b1a8f/demo/main.cpp
			size_t index_count = mesh->indices.size();
			size_t vertex_count = mesh->vertex_positions.size();

			const size_t lod_count = (size_t)lodCountSlider.GetValue();
			struct LOD
			{
				struct Subset
				{
					wi::vector<uint32_t> indices;
				};
				wi::vector<Subset> subsets;
			};
			wi::vector<LOD> lods(lod_count);

			const float target_error = lodErrorSlider.GetValue();

			for (size_t i = 0; i < lod_count; ++i)
			{
				lods[i].subsets.resize(mesh->subsets_per_lod);
				for (uint32_t subsetIndex = 0; subsetIndex < mesh->subsets_per_lod; ++subsetIndex)
				{
					const MeshComponent::MeshSubset& subset = mesh->subsets[subsetIndex];
					lods[i].subsets[subsetIndex].indices.resize(subset.indexCount);
					for (uint32_t ind = 0; ind < subset.indexCount; ++ind)
					{
						lods[i].subsets[subsetIndex].indices[ind] = mesh->indices[subset.indexOffset + ind];
					}
				}
			}

			for (uint32_t subsetIndex = 0; subsetIndex < mesh->subsets_per_lod; ++subsetIndex)
			{
				const MeshComponent::MeshSubset& subset = mesh->subsets[subsetIndex];

				float threshold = wi::math::Lerp(0, 0.9f, wi::math::saturate(lodQualitySlider.GetValue()));
				for (size_t i = 1; i < lod_count; ++i)
				{
					wi::vector<unsigned int>& lod = lods[i].subsets[subsetIndex].indices;

					size_t target_index_count = size_t(mesh->indices.size() * threshold) / 3 * 3;

					// we can simplify all the way from base level or from the last result
					// simplifying from the base level sometimes produces better results, but simplifying from last level is faster
					//const wi::vector<unsigned int>& source = lods[0].subsets[subsetIndex].indices;
					const wi::vector<unsigned int>& source = lods[i - 1].subsets[subsetIndex].indices;

					if (source.size() < target_index_count)
						target_index_count = source.size();

					lod.resize(source.size());
					if (lodSloppyCheckBox.GetCheck())
					{
						lod.resize(meshopt_simplifySloppy(&lod[0], &source[0], source.size(), &mesh->vertex_positions[0].x, mesh->vertex_positions.size(), sizeof(XMFLOAT3), target_index_count, target_error));
					}
					else
					{
						lod.resize(meshopt_simplify(&lod[0], &source[0], source.size(), &mesh->vertex_positions[0].x, mesh->vertex_positions.size(), sizeof(XMFLOAT3), target_index_count, target_error));
					}

					threshold *= threshold;
				}

				// optimize each individual LOD for vertex cache & overdraw
				for (size_t i = 0; i < lod_count; ++i)
				{
					wi::vector<unsigned int>& lod = lods[i].subsets[subsetIndex].indices;

					meshopt_optimizeVertexCache(&lod[0], &lod[0], lod.size(), mesh->vertex_positions.size());
					meshopt_optimizeOverdraw(&lod[0], &lod[0], lod.size(), &mesh->vertex_positions[0].x, mesh->vertex_positions.size(), sizeof(XMFLOAT3), 1.0f);
				}
			}

			mesh->indices.clear();
			wi::vector<MeshComponent::MeshSubset> subsets;
			for (size_t i = 0; i < lod_count; ++i)
			{
				for (uint32_t subsetIndex = 0; subsetIndex < mesh->subsets_per_lod; ++subsetIndex)
				{
					const MeshComponent::MeshSubset& subset = mesh->subsets[subsetIndex];
					subsets.emplace_back();
					subsets.back() = subset;
					subsets.back().indexOffset = (uint32_t)mesh->indices.size();
					subsets.back().indexCount = (uint32_t)lods[i].subsets[subsetIndex].indices.size();
					for (auto& x : lods[i].subsets[subsetIndex].indices)
					{
						mesh->indices.push_back(x);
					}
				}
			}
			mesh->subsets = subsets;

			mesh->CreateRenderData();
			SetEntity(entity, subset);
		}
		});
	AddWidget(&lodgenButton);

	lodCountSlider.Create(2, 10, 6, 8, "LOD Count: ");
	lodCountSlider.SetTooltip("This is how many levels of detail will be created.");
	lodCountSlider.SetSize(XMFLOAT2(wid, hei));
	lodCountSlider.SetPos(XMFLOAT2(x, y += step));
	AddWidget(&lodCountSlider);

	lodQualitySlider.Create(0.1f, 1.0f, 0.5f, 10000, "LOD Quality: ");
	lodQualitySlider.SetTooltip("Lower values will make LODs more agressively simplified.");
	lodQualitySlider.SetSize(XMFLOAT2(wid, hei));
	lodQualitySlider.SetPos(XMFLOAT2(x, y += step));
	AddWidget(&lodQualitySlider);

	lodErrorSlider.Create(0.01f, 0.1f, 0.03f, 10000, "LOD Error: ");
	lodErrorSlider.SetTooltip("Lower values will make more precise levels of detail.");
	lodErrorSlider.SetSize(XMFLOAT2(wid, hei));
	lodErrorSlider.SetPos(XMFLOAT2(x, y += step));
	AddWidget(&lodErrorSlider);

	lodSloppyCheckBox.Create("Sloppy LOD: ");
	lodSloppyCheckBox.SetTooltip("Use the sloppy simplification algorithm, which is faster but doesn't preserve shape well.");
	lodSloppyCheckBox.SetSize(XMFLOAT2(hei, hei));
	lodSloppyCheckBox.SetPos(XMFLOAT2(x, y += step));
	AddWidget(&lodSloppyCheckBox);


	SetMinimized(true);
	SetVisible(false);

	SetEntity(INVALID_ENTITY, -1);
}

void MeshWindow::SetEntity(Entity entity, int subset)
{
	subset = std::max(0, subset);

	this->entity = entity;
	this->subset = subset;

	Scene& scene = editor->GetCurrentScene();

	const MeshComponent* mesh = scene.meshes.GetComponent(entity);

	if (mesh != nullptr)
	{
		const NameComponent& name = *scene.names.GetComponent(entity);

		std::string ss;
		ss += "Mesh name: " + name.name + "\n";
		ss += "Vertex count: " + std::to_string(mesh->vertex_positions.size()) + "\n";
		ss += "Index count: " + std::to_string(mesh->indices.size()) + "\n";
		ss += "Subset count: " + std::to_string(mesh->subsets.size()) + " (" + std::to_string(mesh->GetLODCount()) + " LODs)\n";
		ss += "GPU memory: " + std::to_string((mesh->generalBuffer.GetDesc().size + mesh->streamoutBuffer.GetDesc().size) / 1024.0f / 1024.0f) + " MB\n";
		ss += "\nVertex buffers:\n";
		if (!mesh->vertex_positions.empty()) ss += "\tposition;\n";
		if (!mesh->vertex_normals.empty()) ss += "\tnormal;\n";
		if (!mesh->vertex_windweights.empty()) ss += "\twind;\n";
		if (mesh->vb_uvs.IsValid()) ss += "\tuvsets;\n";
		if (mesh->vb_atl.IsValid()) ss += "\tatlas;\n";
		if (mesh->vb_col.IsValid()) ss += "\tcolor;\n";
		if (mesh->so_pre.IsValid()) ss += "\tprevious_position;\n";
		if (mesh->vb_bon.IsValid()) ss += "\tbone;\n";
		if (mesh->vb_tan.IsValid()) ss += "\ttangent;\n";
		if (mesh->so_pos_nor_wind.IsValid()) ss += "\tstreamout_position;\n";
		if (mesh->so_tan.IsValid()) ss += "\tstreamout_tangents;\n";
		meshInfoLabel.SetText(ss);

		subsetComboBox.ClearItems();
		for (size_t i = 0; i < mesh->subsets.size(); ++i)
		{
			subsetComboBox.AddItem(std::to_string(i));
		}
		if (subset >= 0)
		{
			subsetComboBox.SetSelectedWithoutCallback(subset);
		}

		subsetMaterialComboBox.ClearItems();
		subsetMaterialComboBox.AddItem("NO MATERIAL");
		for (size_t i = 0; i < scene.materials.GetCount(); ++i)
		{
			Entity entity = scene.materials.GetEntity(i);
			const NameComponent& name = *scene.names.GetComponent(entity);
			subsetMaterialComboBox.AddItem(name.name);

			if (subset >= 0 && subset < mesh->subsets.size() && mesh->subsets[subset].materialID == entity)
			{
				subsetMaterialComboBox.SetSelected((int)i + 1);
			}
		}

		doubleSidedCheckBox.SetCheck(mesh->IsDoubleSided());

		const ImpostorComponent* impostor = scene.impostors.GetComponent(entity);
		if (impostor != nullptr)
		{
			impostorCreateButton.SetText("Delete Impostor");
			impostorDistanceSlider.SetValue(impostor->swapInDistance);
		}
		else
		{
			impostorCreateButton.SetText("Create Impostor");
		}
		tessellationFactorSlider.SetValue(mesh->GetTessellationFactor());

		softbodyCheckBox.SetCheck(false);

		SoftBodyPhysicsComponent* physicscomponent = editor->GetCurrentScene().softbodies.GetComponent(entity);
		if (physicscomponent != nullptr)
		{
			softbodyCheckBox.SetCheck(true);
			massSlider.SetValue(physicscomponent->mass);
			frictionSlider.SetValue(physicscomponent->friction);
			restitutionSlider.SetValue(physicscomponent->restitution);
		}

		uint8_t selected = morphTargetCombo.GetSelected();
		morphTargetCombo.ClearItems();
		for (size_t i = 0; i < mesh->morph_targets.size(); i++)
		{
			morphTargetCombo.AddItem(std::to_string(i).c_str());
		}
		if (selected < mesh->morph_targets.size())
		{
			morphTargetCombo.SetSelected(selected);
		}
		SetEnabled(true);

		if (mesh->morph_targets.empty())
		{
			morphTargetCombo.SetEnabled(false);
			morphTargetSlider.SetEnabled(false);
		}
		else
		{
			morphTargetCombo.SetEnabled(true);
			morphTargetSlider.SetEnabled(true);
		}
	}
	else
	{
		meshInfoLabel.SetText("Select a mesh...");
		SetEnabled(false);
	}

	mergeButton.SetEnabled(true);
}
