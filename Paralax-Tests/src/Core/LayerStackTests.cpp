#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/LayerStack.h>

#include <string>
#include <vector>

using namespace Paralax;

namespace {

	class TrackedLayer : public Layer
	{
	public:
		explicit TrackedLayer(std::string name, std::vector<std::string>* destroyed = nullptr)
			: Layer(std::move(name))
			, m_Destroyed(destroyed)
		{
		}

		~TrackedLayer() override
		{
			if (m_Destroyed)
				m_Destroyed->push_back(GetName());
		}

	private:
		std::vector<std::string>* m_Destroyed;
	};

	std::vector<std::string> Names(const LayerStack& stack)
	{
		std::vector<std::string> names;
		for (const auto& layer : stack)
			names.push_back(layer->GetName());
		return names;
	}

}

TEST_CASE("LayerStack: layers are ordered before overlays regardless of push order")
{
	LayerStack stack;
	stack.PushOverlay(CreateScope<TrackedLayer>("Overlay1"));
	stack.PushLayer(CreateScope<TrackedLayer>("Layer1"));
	stack.PushOverlay(CreateScope<TrackedLayer>("Overlay2"));
	stack.PushLayer(CreateScope<TrackedLayer>("Layer2"));

	CHECK(Names(stack) == std::vector<std::string>{"Layer1", "Layer2", "Overlay1", "Overlay2"});
	CHECK(stack.Size() == 4);
}

TEST_CASE("LayerStack: pop returns ownership and keeps the layer/overlay boundary")
{
	LayerStack stack;
	Layer* layer1 = stack.PushLayer(CreateScope<TrackedLayer>("Layer1"));
	Layer* overlay = stack.PushOverlay(CreateScope<TrackedLayer>("Overlay"));
	stack.PushLayer(CreateScope<TrackedLayer>("Layer2"));

	Scope<Layer> popped = stack.PopLayer(layer1);
	REQUIRE(popped);
	CHECK(popped.get() == layer1);
	CHECK(Names(stack) == std::vector<std::string>{"Layer2", "Overlay"});

	// Inserting after a pop must still place the new layer before the overlays.
	stack.PushLayer(CreateScope<TrackedLayer>("Layer3"));
	CHECK(Names(stack) == std::vector<std::string>{"Layer2", "Layer3", "Overlay"});

	CHECK(stack.PopOverlay(overlay).get() == overlay);
	CHECK(Names(stack) == std::vector<std::string>{"Layer2", "Layer3"});
}

TEST_CASE("LayerStack: popping with the wrong kind or an unknown layer returns null and changes nothing")
{
	LayerStack stack;
	Layer* layer = stack.PushLayer(CreateScope<TrackedLayer>("Layer"));
	Layer* overlay = stack.PushOverlay(CreateScope<TrackedLayer>("Overlay"));
	TrackedLayer stranger("Stranger");

	CHECK(stack.PopOverlay(layer) == nullptr);
	CHECK(stack.PopLayer(overlay) == nullptr);
	CHECK(stack.PopLayer(&stranger) == nullptr);
	CHECK(stack.PopLayer(nullptr) == nullptr);
	CHECK(Names(stack) == std::vector<std::string>{"Layer", "Overlay"});
}

TEST_CASE("LayerStack: destroys owned layers in reverse order")
{
	std::vector<std::string> destroyed;
	{
		LayerStack stack;
		stack.PushLayer(CreateScope<TrackedLayer>("Layer1", &destroyed));
		stack.PushLayer(CreateScope<TrackedLayer>("Layer2", &destroyed));
		stack.PushOverlay(CreateScope<TrackedLayer>("Overlay", &destroyed));
	}
	CHECK(destroyed == std::vector<std::string>{"Overlay", "Layer2", "Layer1"});
}

TEST_CASE("LayerStack: Clear empties the stack and resets the insert position")
{
	LayerStack stack;
	stack.PushLayer(CreateScope<TrackedLayer>("Layer"));
	stack.PushOverlay(CreateScope<TrackedLayer>("Overlay"));
	stack.Clear();
	CHECK(stack.Empty());

	stack.PushOverlay(CreateScope<TrackedLayer>("Overlay"));
	stack.PushLayer(CreateScope<TrackedLayer>("Layer"));
	CHECK(Names(stack) == std::vector<std::string>{"Layer", "Overlay"});
}

TEST_CASE("LayerStack: pushing a null layer or overlay is a verified precondition")
{
	Paralax::Testing::ScopedAssertionCapture capture;
	LayerStack stack;
	CHECK_THROWS_AS(stack.PushLayer(nullptr), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(stack.PushOverlay(nullptr), Paralax::Testing::AssertionFailedError);
	CHECK(stack.Empty());
}
