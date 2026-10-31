#include "EditorCore/EditorCamera.h"

#include <Paralax/Core/Input.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Paralax::Editor {

	EditorCamera::EditorCamera() = default;

	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(m_Pitch, m_Yaw, 0.0f));
	}

	glm::vec3 EditorCamera::GetForward() const
	{
		return GetOrientation() * glm::vec3(0.0f, 0.0f, -1.0f);
	}

	glm::vec3 EditorCamera::GetRight() const
	{
		return GetOrientation() * glm::vec3(1.0f, 0.0f, 0.0f);
	}

	glm::vec3 EditorCamera::GetUp() const
	{
		return GetOrientation() * glm::vec3(0.0f, 1.0f, 0.0f);
	}

	void EditorCamera::SetViewportSize(float width, float height)
	{
		m_ViewportWidth = std::max(width, 1.0f);
		m_ViewportHeight = std::max(height, 1.0f);
	}

	void EditorCamera::OnUpdate(Timestep ts, bool viewportHovered)
	{
		const glm::vec2 mouse = Input::GetMousePosition();
		const glm::vec2 delta = (mouse - m_LastMouse) * 0.003f;
		m_LastMouse = mouse;

		const bool look = Input::IsMouseButtonDown(Mouse::ButtonRight);
		const bool orbit = Input::IsMouseButtonDown(Mouse::ButtonLeft) && (Input::IsKeyDown(Key::LeftAlt) || Input::IsKeyDown(Key::RightAlt));
		const bool pan = Input::IsMouseButtonDown(Mouse::ButtonMiddle);

		// Start controlling only when a drag begins over the viewport; keep control until the buttons are released.
		if (!m_Controlling)
			m_Controlling = viewportHovered && (look || orbit || pan);
		if (!(look || orbit || pan))
			m_Controlling = false;
		if (!m_Controlling)
			return;

		if (look)
		{
			// Rotate around the camera position: keep the eye fixed by moving the focal point.
			const glm::vec3 position = GetPosition();
			m_Yaw -= delta.x * 0.8f;
			m_Pitch = std::clamp(m_Pitch - delta.y * 0.8f, glm::radians(-89.0f), glm::radians(89.0f));
			m_FocalPoint = position + GetForward() * m_Distance;

			glm::vec3 move(0.0f);
			if (Input::IsKeyDown(Key::W))
				move += GetForward();
			if (Input::IsKeyDown(Key::S))
				move -= GetForward();
			if (Input::IsKeyDown(Key::D))
				move += GetRight();
			if (Input::IsKeyDown(Key::A))
				move -= GetRight();
			if (Input::IsKeyDown(Key::E))
				move += glm::vec3(0.0f, 1.0f, 0.0f);
			if (Input::IsKeyDown(Key::Q))
				move -= glm::vec3(0.0f, 1.0f, 0.0f);
			if (glm::dot(move, move) > 0.0f)
			{
				const float speed = (Input::IsKeyDown(Key::LeftShift) ? 15.0f : 5.0f) * std::max(m_Distance * 0.2f, 1.0f);
				m_FocalPoint += glm::normalize(move) * speed * ts.GetSeconds();
			}
		}
		else if (orbit)
		{
			m_Yaw -= delta.x * 0.8f;
			m_Pitch = std::clamp(m_Pitch - delta.y * 0.8f, glm::radians(-89.0f), glm::radians(89.0f));
		}
		else if (pan)
		{
			const float speed = m_Distance * 0.6f;
			m_FocalPoint += (-GetRight() * delta.x + GetUp() * delta.y) * speed;
		}
	}

	void EditorCamera::OnScroll(float delta)
	{
		const float step = std::max(m_Distance * 0.12f, 0.05f) * delta;
		m_Distance -= step;
		if (m_Distance < 0.25f)
		{
			// Push the focal point forward instead of passing through it.
			m_FocalPoint += GetForward() * (0.25f - m_Distance);
			m_Distance = 0.25f;
		}
	}

	void EditorCamera::Focus(const glm::vec3& center, float radius)
	{
		m_FocalPoint = center;
		m_Distance = std::max(radius, 0.25f) / std::tan(m_VerticalFOV * 0.5f) * 1.2f;
	}

	void EditorCamera::LookAt(const glm::vec3& position, const glm::vec3& target)
	{
		const glm::vec3 offset = target - position;
		const float distance = glm::length(offset);
		if (!(distance > 1e-4f) || !std::isfinite(distance))
			return;
		const glm::vec3 direction = offset / distance;
		// Inverse of GetForward: forward = Ry(yaw) * Rx(pitch) * -Z.
		m_Pitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
		m_Yaw = std::atan2(-direction.x, -direction.z);
		m_FocalPoint = target;
		m_Distance = distance;
	}

	glm::mat4 EditorCamera::GetViewMatrix() const
	{
		return glm::lookAtRH(GetPosition(), m_FocalPoint, glm::vec3(0.0f, 1.0f, 0.0f) * (std::cos(m_Pitch) >= 0.0f ? 1.0f : -1.0f));
	}

	CameraView EditorCamera::GetView() const
	{
		CameraView view;
		view.View = GetViewMatrix();
		view.Projection = glm::perspectiveRH_ZO(m_VerticalFOV, m_ViewportWidth / m_ViewportHeight, m_Far, m_Near);
		view.Position = GetPosition();
		view.Near = m_Near;
		view.Far = m_Far;
		view.VerticalFOV = m_VerticalFOV;
		return view;
	}

	glm::mat4 EditorCamera::GetGizmoProjection() const
	{
		return glm::perspectiveRH_NO(m_VerticalFOV, m_ViewportWidth / m_ViewportHeight, m_Near, m_Far);
	}

}
