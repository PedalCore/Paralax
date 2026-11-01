#include "pxpch.h"
#include "Paralax/Game/GameState.h"

#include <cmath>

namespace Paralax {

	size_t GameState::SizeOf(const std::string& key, const Value& value)
	{
		const std::string* text = std::get_if<std::string>(&value);
		return key.size() + (text ? text->size() : sizeof(double));
	}

	bool GameState::Set(std::string_view key, Value value)
	{
		if (key.empty() || key.size() > MaxKeyBytes)
			return false;
		if (const double* number = std::get_if<double>(&value); number && !std::isfinite(*number))
			return false;
		if (const std::string* text = std::get_if<std::string>(&value); text && text->size() > MaxStringBytes)
			return false;
		auto it = m_Values.find(key);
		const std::string name(key);
		const size_t replaced = it != m_Values.end() ? SizeOf(it->first, it->second) : 0;
		const size_t added = SizeOf(name, value);
		if (m_Bytes - replaced + added > MaxTotalBytes)
			return false;
		if (it == m_Values.end())
		{
			if (m_Values.size() >= MaxKeys)
				return false;
			m_Values.emplace(name, std::move(value));
		}
		else
			it->second = std::move(value);
		m_Bytes = m_Bytes - replaced + added;
		return true;
	}

	std::optional<GameState::Value> GameState::Get(std::string_view key) const
	{
		auto it = m_Values.find(key);
		if (it == m_Values.end())
			return std::nullopt;
		return it->second;
	}

	bool GameState::Remove(std::string_view key)
	{
		auto it = m_Values.find(key);
		if (it == m_Values.end())
			return false;
		m_Bytes -= SizeOf(it->first, it->second);
		m_Values.erase(it);
		return true;
	}

}
