#pragma once
#include "WickedEngine.h"

class EditorComponent;

class SpringWindow : public wi::gui::Window
{
public:
	void Create(EditorComponent* editor);

	EditorComponent* editor = nullptr;
	wi::ecs::Entity entity;
	void SetEntity(wi::ecs::Entity entity);

	wi::gui::CheckBox debugCheckBox;
	wi::gui::CheckBox disabledCheckBox;
	wi::gui::CheckBox stretchCheckBox;
	wi::gui::CheckBox gravityCheckBox;
	wi::gui::Slider stiffnessSlider;
	wi::gui::Slider dampingSlider;
	wi::gui::Slider windSlider;
};

