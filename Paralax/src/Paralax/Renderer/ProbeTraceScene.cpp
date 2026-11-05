#include "pxpch.h"
#include "Paralax/Renderer/ProbeTraceScene.h"

namespace Paralax {

	ProbeTraceScene::ProbeTraceScene() = default;

	ProbeTraceScene::ProbeTraceScene(const GraphicsDeviceLimits& limits)
		: m_Source(limits)
	{
	}

	bool ProbeTraceScene::Update(Scene& scene, AssetManager& assets, bool& changed)
	{
		return m_Source.UpdateImpl(scene, assets, changed, ProbeTraceSource::RepresentationRequirement::Legacy);
	}

	const Ref<const ProbeTraceSceneIdentity>& ProbeTraceScene::GetSceneIdentity() const
	{
		static const Ref<const ProbeTraceSceneIdentity> empty;
		const auto* snapshot = m_Source.GetAcceptedSnapshot();
		return snapshot ? snapshot->GetSceneIdentity() : empty;
	}

	const std::vector<GPU::ProbeTraceNode>& ProbeTraceScene::GetNodes() const
	{
		static const std::vector<GPU::ProbeTraceNode> empty;
		const auto* legacy = m_Source.GetAcceptedLegacy();
		return legacy ? legacy->GetNodes() : empty;
	}

	const std::vector<GPU::ProbeTraceTriangle>& ProbeTraceScene::GetTriangles() const
	{
		static const std::vector<GPU::ProbeTraceTriangle> empty;
		const auto* legacy = m_Source.GetAcceptedLegacy();
		return legacy ? legacy->GetTriangles() : empty;
	}

	const std::vector<GPU::ProbeTraceMaterial>& ProbeTraceScene::GetMaterials() const
	{
		static const std::vector<GPU::ProbeTraceMaterial> empty;
		const auto* snapshot = m_Source.GetAcceptedSnapshot();
		return snapshot ? snapshot->GetMaterials() : empty;
	}

	const std::vector<glm::vec4>& ProbeTraceScene::GetTexels() const
	{
		static const std::vector<glm::vec4> empty;
		const auto* snapshot = m_Source.GetAcceptedSnapshot();
		return snapshot ? snapshot->GetTexels() : empty;
	}

}
