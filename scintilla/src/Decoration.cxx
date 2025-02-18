/** @file Decoration.cxx
 ** Visual elements added over text.
 **/
// Copyright 1998-2007 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include <stdexcept>
#include <string_view>
#include <vector>
#include <optional>
#include <algorithm>
#include <memory>

#include "ScintillaTypes.h"

#include "Debugging.h"

#include "Position.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "Decoration.h"

using namespace Scintilla::Internal;

namespace {

template <typename POS>
class Decoration : public IDecoration {
	int indicator;
public:
	RunStyles<POS, int> rs;

	explicit Decoration(int indicator_) : indicator(indicator_) {}

	bool Empty() const noexcept override {
		return (rs.Runs() == 1) && (rs.AllSameAs(0));
	}
	int Indicator() const noexcept override {
		return indicator;
	}
	Sci::Position Length() const noexcept override {
		return rs.Length();
	}
	int ValueAt(Sci::Position position) const noexcept override {
		return rs.ValueAt(static_cast<POS>(position));
	}
	Sci::Position StartRun(Sci::Position position) const noexcept override {
		return rs.StartRun(static_cast<POS>(position));
	}
	Sci::Position EndRun(Sci::Position position) const noexcept override {
		return rs.EndRun(static_cast<POS>(position));
	}
	void SetValueAt(Sci::Position position, int value) override {
		rs.SetValueAt(static_cast<POS>(position), value);
	}
	void InsertSpace(Sci::Position position, Sci::Position insertLength) override {
		rs.InsertSpace(static_cast<POS>(position), static_cast<POS>(insertLength));
	}
	Sci::Position Runs() const noexcept override {
		return rs.Runs();
	}
};

template <typename POS>
class DecorationList : public IDecorationList {
	int currentIndicator;
	int currentValue;
	Decoration<POS> *current;	// Non-owning. Cached so FillRange doesn't have to search for each call.
	Sci::Position lengthDocument;
	// Ordered by indicator
	std::vector<std::unique_ptr<Decoration<POS>>> decorationList;
	std::vector<const IDecoration*> decorationView;	// Read-only view of decorationList
	bool clickNotified;

	Decoration<POS> *DecorationFromIndicator(int indicator) noexcept;
	Decoration<POS> *Create(int indicator, Sci::Position length);
	void Delete(int indicator);
	void DeleteAnyEmpty();
	void SetView();
public:

	DecorationList() noexcept;

	const std::vector<const IDecoration*> &View() const noexcept override {
		return decorationView;
	}

	void SetCurrentIndicator(int indicator) noexcept override;
	int GetCurrentIndicator() const noexcept override {
		return currentIndicator;
	}

	void SetCurrentValue(int value) noexcept override;
	int GetCurrentValue() const noexcept override {
		return currentValue;
	}

	// Returns changed=true if some values may have changed
	FillResult<Sci::Position> FillRange(Sci::Position position, int value, Sci::Position fillLength) override;

	void InsertSpace(Sci::Position position, Sci::Position insertLength) override;
	void DeleteRange(Sci::Position position, Sci::Position deleteLength) override;

	void DeleteLexerDecorations() override;

	int AllOnFor(Sci::Position position) const noexcept override;
	int ValueAt(int indicator, Sci::Position position) noexcept override;
	Sci::Position Start(int indicator, Sci::Position position) noexcept override;
	Sci::Position End(int indicator, Sci::Position position) noexcept override;

	bool ClickNotified() const noexcept override {
		return clickNotified;
	}
	void SetClickNotified(bool notified) noexcept override {
		clickNotified = notified;
	}
};

template <typename POS>
DecorationList<POS>::DecorationList() noexcept : currentIndicator(0), currentValue(1), current(nullptr),
lengthDocument(0), clickNotified(false) {
}

template <typename POS>
Decoration<POS> *DecorationList<POS>::DecorationFromIndicator(int indicator) noexcept {
	for (const auto &deco : decorationList) {
		if (deco->Indicator() == indicator) {
			return deco.get();
		}
	}
	return nullptr;
}

template <typename POS>
Decoration<POS> *DecorationList<POS>::Create(int indicator, Sci::Position length) {
	currentIndicator = indicator;
	auto decoNew = std::make_unique<Decoration<POS>>(indicator);
	decoNew->rs.InsertSpace(0, static_cast<POS>(length));

	auto it = std::lower_bound(
		decorationList.begin(), decorationList.end(), decoNew,
		[](const std::unique_ptr<Decoration<POS>> &a, const std::unique_ptr<Decoration<POS>> &b) noexcept {
		return a->Indicator() < b->Indicator();
	});
	const auto itAdded = decorationList.insert(it, std::move(decoNew));

	SetView();

	return itAdded->get();
}

template <typename POS>
void DecorationList<POS>::Delete(int indicator) {
	decorationList.erase(std::remove_if(decorationList.begin(), decorationList.end(),
		[indicator](const std::unique_ptr<Decoration<POS>> &deco) noexcept {
		return deco->Indicator() == indicator;
	}), decorationList.end());
	current = nullptr;
	SetView();
}

template <typename POS>
void DecorationList<POS>::SetCurrentIndicator(int indicator) noexcept {
	currentIndicator = indicator;
	current = DecorationFromIndicator(indicator);
	currentValue = 1;
}

template <typename POS>
void DecorationList<POS>::SetCurrentValue(int value) noexcept {
	currentValue = value ? value : 1;
}

template <typename POS>
FillResult<Sci::Position> DecorationList<POS>::FillRange(Sci::Position position, int value, Sci::Position fillLength) {
	if (!current) {
		current = DecorationFromIndicator(currentIndicator);
		if (!current) {
			current = Create(currentIndicator, lengthDocument);
		}
	}
	// Converting result from POS to Sci::Position as callers not polymorphic.
	const FillResult<POS> frInPOS = current->rs.FillRange(static_cast<POS>(position), value, static_cast<POS>(fillLength));
	const FillResult<Sci::Position> fr{ frInPOS.changed, frInPOS.position, frInPOS.fillLength };
	if (current->Empty()) {
		Delete(currentIndicator);
	}
	return fr;
}

template <typename POS>
void DecorationList<POS>::InsertSpace(Sci::Position position, Sci::Position insertLength) {
	const bool atEnd = position == lengthDocument;
	lengthDocument += insertLength;
	for (const auto &deco : decorationList) {
		deco->rs.InsertSpace(static_cast<POS>(position), static_cast<POS>(insertLength));
		if (atEnd) {
			deco->rs.FillRange(static_cast<POS>(position), 0, static_cast<POS>(insertLength));
		}
	}
}

template <typename POS>
void DecorationList<POS>::DeleteRange(Sci::Position position, Sci::Position deleteLength) {
	lengthDocument -= deleteLength;
	for (const auto &deco : decorationList) {
		deco->rs.DeleteRange(static_cast<POS>(position), static_cast<POS>(deleteLength));
	}
	DeleteAnyEmpty();
	if (decorationList.size() != decorationView.size()) {
		// One or more empty decorations deleted so update view.
		current = nullptr;
		SetView();
	}
}

template <typename POS>
void DecorationList<POS>::DeleteLexerDecorations() {
	decorationList.erase(std::remove_if(decorationList.begin(), decorationList.end(),
		[](const std::unique_ptr<Decoration<POS>> &deco) noexcept {
		return deco->Indicator() < static_cast<int>(Scintilla::IndicatorNumbers::Container);
	}), decorationList.end());
	current = nullptr;
	SetView();
}

template <typename POS>
void DecorationList<POS>::DeleteAnyEmpty() {
	if (lengthDocument == 0) {
		decorationList.clear();
	} else {
		decorationList.erase(std::remove_if(decorationList.begin(), decorationList.end(),
			[](const std::unique_ptr<Decoration<POS>> &deco) noexcept {
			return deco->Empty();
		}), decorationList.end());
	}
}

template <typename POS>
void DecorationList<POS>::SetView() {
	decorationView.resize(decorationList.size());
	auto iter = decorationView.begin();
	for (const auto &deco : decorationList) {
		*iter++ = deco.get();
	}
}

template <typename POS>
int DecorationList<POS>::AllOnFor(Sci::Position position) const noexcept {
	int mask = 0;
	for (const auto &deco : decorationList) {
		if (deco->rs.ValueAt(static_cast<POS>(position))) {
			if (deco->Indicator() < static_cast<int>(Scintilla::IndicatorNumbers::Ime)) {
				mask |= 1u << deco->Indicator();
			}
		}
	}
	return mask;
}

template <typename POS>
int DecorationList<POS>::ValueAt(int indicator, Sci::Position position) noexcept {
	const auto *deco = DecorationFromIndicator(indicator);
	if (deco) {
		return deco->rs.ValueAt(static_cast<POS>(position));
	}
	return 0;
}

template <typename POS>
Sci::Position DecorationList<POS>::Start(int indicator, Sci::Position position) noexcept {
	const auto *deco = DecorationFromIndicator(indicator);
	if (deco) {
		return deco->rs.StartRun(static_cast<POS>(position));
	}
	return 0;
}

template <typename POS>
Sci::Position DecorationList<POS>::End(int indicator, Sci::Position position) noexcept {
	const auto *deco = DecorationFromIndicator(indicator);
	if (deco) {
		return deco->rs.EndRun(static_cast<POS>(position));
	}
	return 0;
}

}

namespace Scintilla::Internal {

std::unique_ptr<IDecoration> DecorationCreate(bool largeDocument, int indicator) {
	if (largeDocument)
		return std::make_unique<Decoration<Sci::Position>>(indicator);
	else
		return std::make_unique<Decoration<int>>(indicator);
}

std::unique_ptr<IDecorationList> DecorationListCreate(bool largeDocument) {
	if (largeDocument)
		return std::make_unique<DecorationList<Sci::Position>>();
	else
		return std::make_unique<DecorationList<int>>();
}

}

