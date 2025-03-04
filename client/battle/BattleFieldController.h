/*
 * BattleFieldController.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/battle/BattleHex.h"
#include "../gui/CIntObject.h"

VCMI_LIB_NAMESPACE_BEGIN

class CStack;

VCMI_LIB_NAMESPACE_END

struct Rect;
struct Point;

class ClickableHex;
class BattleHero;
class Canvas;
class IImage;
class BattleInterface;

/// Handles battlefield grid as well as rendering of background layer of battle interface
class BattleFieldController : public CIntObject
{
	BattleInterface & owner;

	std::shared_ptr<IImage> background;
	std::shared_ptr<IImage> cellBorder;
	std::shared_ptr<IImage> cellShade;

	/// Canvas that contains background, hex grid (if enabled), absolute obstacles and movement range of active stack
	std::unique_ptr<Canvas> backgroundWithHexes;

	/// Canvas that contains cell borders of all tiles in the battlefield
	std::unique_ptr<Canvas> cellBorders;

	/// hex from which the stack would perform attack with current cursor
	BattleHex attackingHex;

	/// hexes to which currently active stack can move
	std::vector<BattleHex> occupyableHexes;

	/// hexes that when in front of a unit cause it's amount box to move back
	std::array<bool, GameConstants::BFIELD_SIZE> stackCountOutsideHexes;

	std::vector<std::shared_ptr<ClickableHex>> bfield;

	void showHighlightedHex(Canvas & to, BattleHex hex, bool darkBorder);

	std::set<BattleHex> getHighlightedHexesStackRange();
	std::set<BattleHex> getHighlightedHexesSpellRange();
	std::set<BattleHex> getHighlightedHexesMovementTarget();

	void showBackground(Canvas & canvas);
	void showBackgroundImage(Canvas & canvas);
	void showBackgroundImageWithHexes(Canvas & canvas);
	void showHighlightedHexes(Canvas & canvas);

	BattleHex::EDir selectAttackDirection(BattleHex myNumber, const Point & point);

	void mouseMoved(const SDL_MouseMotionEvent &event) override;
	void showAll(SDL_Surface * to) override;
	void show(SDL_Surface * to) override;

public:
	BattleFieldController(BattleInterface & owner);

	void createHeroes();

	void redrawBackgroundWithHexes();
	void renderBattlefield(Canvas & canvas);

	/// Returns position of hex relative to owner (BattleInterface)
	Rect hexPositionLocal(BattleHex hex) const;

	/// Returns position of hex relative to game window
	Rect hexPositionAbsolute(BattleHex hex) const;

	/// Checks whether selected pixel is transparent, uses local coordinates of a hex
	bool isPixelInHex(Point const & position);

	/// Returns ID of currently hovered hex or BattleHex::INVALID if none
	BattleHex getHoveredHex();

	/// returns true if selected tile can be attacked in melee by current stack
	bool isTileAttackable(const BattleHex & number) const;

	/// returns true if stack should render its stack count image in default position - outside own hex
	bool stackCountOutsideHex(const BattleHex & number) const;

	void setBattleCursor(BattleHex myNumber);
	BattleHex fromWhichHexAttack(BattleHex myNumber);
};
