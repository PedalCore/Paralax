#pragma once

#include <Paralax/Core/Timestep.h>
#include <Paralax/Renderer/SceneRenderer.h>

#include <glm/glm.hpp>

namespace Paralax::Editor {

	// Free-look/orbit camera for the scene viewport.
	//   Right mouse + drag: look around; with W/A/S/D/Q/E: fly (Shift = faster)
	//   Alt + left mouse:   orbit around the focal point
	//   Middle mouse:       pan
	//   Scroll:             dolly towards the focal point
	class EditorCamera
	{
	public:
		EditorCamera();

		// Applies input for this frame. Only call while the viewport is hovered or being dragged.
		void OnUpdate(Timestep ts, bool viewportHovered);
		void OnScroll(float delta);

		// Moves the camera to frame a sphere.
		void Focus(const glm::vec3& center, float radius);
		// Places the camera at position looking at target (which becomes the orbit focal point). Ignored if the two
		// points coincide.
		void LookAt(const glm::vec3& position, const glm::vec3& target);
		const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }
		void SetViewportSize(float width, float height);

		CameraView GetView() const;
		glm::mat4 GetViewMatrix() const;
		// Conventional (non-reversed, OpenGL-style) projection for tools such as ImGuizmo.
		glm::mat4 GetGizmoProjection() const;

		glm::vec3 GetPosition() const { return m_FocalPoint - GetForward() * m_Distance; }
		glm::vec3 GetForward() const;
		glm::vec3 GetRight() const;
		glm::vec3 GetUp() const;

		float GetVerticalFOV() const { return m_VerticalFOV; }
		bool IsControlling() const { return m_Controlling; }

	private:
		glm::quat GetOrientation() const;

	private:
		glm::vec3 m_FocalPoint{0.0f, 0.5f, 0.0f};
		float m_Distance = 8.0f;
		float m_Yaw = glm::radians(-30.0f);
		float m_Pitch = glm::radians(-20.0f);
		float m_VerticalFOV = glm::radians(50.0f);
		float m_Near = 0.05f;
		float m_Far = 2000.0f;
		float m_ViewportWidth = 1280.0f;
		float m_ViewportHeight = 720.0f;
		glm::vec2 m_LastMouse{0.0f};
		bool m_Controlling = false;
	};

}
