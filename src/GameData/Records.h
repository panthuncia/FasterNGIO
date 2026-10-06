#pragma once

#include "GameData/FormID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace FasterNGIO::GameData
{
	// What FasterNGIO reads from each record type: only the fields grass placement and rejection use.

	inline constexpr float kSkyrimTerrainCellSize = 4096.0f;
	// LAND's texture slots refer to this LTEX when they hold 0.
	inline constexpr FormID kSkyrimDefaultLandTextureFormID{ 0x00000C16u };

	// OBND: object-space bounds in game units, as authored (min xyz, max xyz).
	struct ObjectBounds
	{
		std::array<std::int16_t, 3> min{};
		std::array<std::int16_t, 3> max{};
		bool present{ false };
	};

	// STAT, MSTT, TREE, ACTI, DOOR, CONT, FURN, LIGH, FLOR, SCOL and TACT: anything a reference can
	// place with collision.
	struct BaseObjectInfo
	{
		FormID formID{};
		// The record type (STAT, TREE, ...).
		FourCC signature{ 0 };
		std::string editorID;
		std::string modelPath;
		ObjectBounds bounds;
		// MODS: the texture set of each alternate texture of the model, in order (Seasons of
		// Skyrim's snow tests read them).
		std::vector<FormID> alternateTextureSets;
		// STAT DNAM: the directional material object.
		FormID materialObject{};
	};

	struct WorldInfo
	{
		FormID formID{};
		std::string editorID;
		// WNAM: the parent worldspace.
		std::optional<FormID> parentWorldFormID;
		// PNAM: what the worldspace takes from its parent; bit 0 (Use Land Data) includes the default
		// water height.
		std::uint16_t parentUseFlags{ 0 };
		// DNAM's second float; the engine's default is 0.
		float defaultWaterHeight{ 0.0f };
		// DNAM's first float. A LAND without VHGT is flat at this height (Bittercup's ccKRTSSE001QNWorld:
		// 147 of 182 LANDs; NGIO's in-game cache puts their seagrass at exactly -1024, the DNAM land height).
		std::optional<float> defaultLandHeight;

		[[nodiscard]] bool UsesParentLandData() const { return parentWorldFormID.has_value() && (parentUseFlags & 0x1u) != 0; }
	};

	struct CellInfo
	{
		FormID formID{};
		std::optional<FormID> worldFormID;
		std::uint16_t cellFlags{ 0 };
		std::optional<std::int32_t> gridX;
		std::optional<std::int32_t> gridY;
		// XCLW as the engine keeps it (rounded to a whole unit); unset when the record has none or a value
		// of at least 2^31 (0x7F7FFFFF and the like), which leave the worldspace's default.
		std::optional<float> waterHeight;

		[[nodiscard]] bool IsInterior() const { return (cellFlags & 0x1u) != 0; }
		[[nodiscard]] bool HasWater() const { return (cellFlags & 0x2u) != 0; }
	};

	struct LandTextureInfo
	{
		FormID formID{};
		std::string editorID;
		// MNAM: the material type (MATT).
		FormID materialType{};
		// GNAM, in order.
		std::vector<FormID> grassFormIDs;
	};

	// MATT: a material type and its name, from which the engine derives its material ID.
	struct MaterialTypeInfo
	{
		FormID formID{};
		std::string editorID;
		// MNAM.
		std::string name;
	};

	// TXST: only the diffuse texture path (TX00).
	struct TextureSetInfo
	{
		FormID formID{};
		std::string editorID;
		std::string diffuse;
	};

	// MATO: only its editor ID.
	struct MaterialObjectInfo
	{
		FormID formID{};
		std::string editorID;
	};

	enum class GrassWaterState : std::uint32_t
	{
		AboveOnlyAtLeast = 0,
		AboveOnlyAtMost = 1,
		BelowOnlyAtLeast = 2,
		BelowOnlyAtMost = 3,
		BothAtLeast = 4,
		BothAtMost = 5,
		BothAtMostAbove = 6,
		BothAtMostBelow = 7
	};

	struct GrassInfo
	{
		FormID formID{};
		std::string modelPath;
		std::uint8_t density{ 0 };
		std::uint8_t minSlopeDegrees{ 0 };
		std::uint8_t maxSlopeDegrees{ 90 };
		std::uint16_t distanceFromWaterLevel{ 0 };
		GrassWaterState underwaterState{ GrassWaterState::AboveOnlyAtLeast };
		float positionRange{ 128.0f };
		float heightRange{ 0.0f };
		float colorRange{ 0.0f };
		// How fast the grass sways; the shader property takes it from the cache group.
		float wavePeriod{ 0.0f };
		std::uint8_t grassFlags{ 0 };
		ObjectBounds bounds;

		[[nodiscard]] bool HasVertexLighting() const { return (grassFlags & 0x1u) != 0; }
		[[nodiscard]] bool HasUniformScaling() const { return (grassFlags & 0x2u) != 0; }
		[[nodiscard]] bool FitsToSlope() const { return (grassFlags & 0x4u) != 0; }
	};

	struct LandBaseTexture
	{
		FormID landTextureFormID{};
		std::uint8_t quadrant{ 0 };
	};

	struct LandAlphaTexture
	{
		FormID landTextureFormID{};
		std::uint8_t quadrant{ 0 };
		std::uint16_t layerIndex{ 0 };
	};

	struct LandVertexAlpha
	{
		std::uint8_t quadrant{ 0 };
		std::uint16_t layerIndex{ 0 };
		std::uint16_t position{ 0 };
		float opacity{ 0.0f };
	};

	struct LandInfo
	{
		static constexpr std::size_t VertexSide = 33;
		static constexpr std::size_t VertexCount = VertexSide * VertexSide;
		static constexpr std::size_t QuadrantVertexSide = 17;
		static constexpr std::size_t QuadrantVertexCount = QuadrantVertexSide * QuadrantVertexSide;
		static constexpr std::size_t QuadrantCount = 4;

		FormID formID{};
		FormID parentCell{};
		// Filled from the parent cell when the snapshot is built.
		std::optional<FormID> worldFormID;
		std::optional<std::int32_t> cellX;
		std::optional<std::int32_t> cellY;
		std::array<float, VertexCount> heights{};
		std::array<std::array<std::uint8_t, 3>, VertexCount> vertexColors{};
		bool hasHeights{ false };
		bool hasVertexColors{ false };
		std::array<std::optional<LandBaseTexture>, QuadrantCount> baseTextures{};
		std::vector<LandAlphaTexture> alphaTextures;
		std::vector<LandVertexAlpha> vertexAlphas;
	};

	// REFR and ACHR.
	struct PlacementInfo
	{
		FormID formID{};
		FormID parentCell{};
		std::optional<FormID> worldFormID;
		FormID baseFormID{};
		std::uint32_t flags{ 0 };
		float position[3]{};
		float rotation[3]{};
		float scale{ 1.0f };

		[[nodiscard]] bool IsInitiallyDisabled() const { return (flags & (1u << 11)) != 0; }
	};

	// A cell's grid coordinates packed into one key (x in the high half).
	[[nodiscard]] constexpr std::uint64_t PackCellCoords(std::int32_t a_x, std::int32_t a_y)
	{
		return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y);
	}

	// An exterior cell of a worldspace.
	struct CellKey
	{
		FormID worldFormID{};
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };

		friend bool operator==(const CellKey&, const CellKey&) = default;
	};

	struct CellKeyHash
	{
		std::size_t operator()(const CellKey& a_key) const noexcept
		{
			auto value = static_cast<std::size_t>(a_key.worldFormID.value);
			value ^= static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.x)) + 0x9e3779b9u + (value << 6) + (value >> 2);
			value ^= static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.y)) + 0x9e3779b9u + (value << 6) + (value >> 2);
			return value;
		}
	};
}
