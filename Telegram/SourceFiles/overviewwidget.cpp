/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014 John Preston, https://desktop.telegram.org
*/
#include "stdafx.h"

#include "lang.h"
#include "window.h"
#include "mainwidget.h"
#include "overviewwidget.h"
#include "boxes/addcontactbox.h"
#include "boxes/confirmbox.h"
#include "boxes/photocropbox.h"
#include "application.h"
#include "gui/filedialog.h"

// flick scroll taken from http://qt-project.org/doc/qt-4.8/demos-embedded-anomaly-src-flickcharm-cpp.html

OverviewInner::OverviewInner(OverviewWidget *overview, ScrollArea *scroll, const PeerData *peer, MediaOverviewType type) : QWidget(0)
	, _overview(overview)
	, _scroll(scroll)
	, _resizeIndex(-1)
	, _resizeSkip(0)
	, _peer(App::peer(peer->id))
	, _type(type)
	, _hist(App::history(peer->id))
	, _photosInRow(1)
	, _photosToAdd(0)
	, _selMode(false)
	, _audioLeft(st::msgMargin.left())
	, _audioWidth(st::msgMinWidth)
	, _audioHeight(st::mediaPadding.top() + st::mediaThumbSize + st::mediaPadding.bottom())
	, _width(st::wndMinWidth)
	, _height(0)
	, _minHeight(0)
	, _addToY(0)
	, _cursor(style::cur_default)
	, _dragAction(NoDrag)
	, _dragItem(0), _selectedMsgId(0)
	, _dragItemIndex(-1)
	, _mousedItem(0)
	, _mousedItemIndex(-1)
	, _dragWasInactive(false)
	, _dragSelFrom(0)
	, _dragSelTo(0)
	, _dragSelecting(false)
	, _touchScroll(false)
	, _touchSelect(false)
	, _touchInProgress(false)
	, _touchScrollState(TouchScrollManual)
	, _touchPrevPosValid(false)
	, _touchWaitingAcceleration(false)
	, _touchSpeedTime(0)
	, _touchAccelerationTime(0)
	, _touchTime(0)
	, _menu(0) {

	resize(_width, height());

	App::contextItem(0);

	_touchSelectTimer.setSingleShot(true);
	connect(&_touchSelectTimer, SIGNAL(timeout()), this, SLOT(onTouchSelect()));

	setAttribute(Qt::WA_AcceptTouchEvents);
	connect(&_touchScrollTimer, SIGNAL(timeout()), this, SLOT(onTouchScrollTimer()));

	mediaOverviewUpdated();
	setMouseTracking(true);

	if (_type == OverviewAudioDocuments) {

	}
}

bool OverviewInner::event(QEvent *e) {
	if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate || e->type() == QEvent::TouchEnd || e->type() == QEvent::TouchCancel) {
		QTouchEvent *ev = static_cast<QTouchEvent*>(e);
		if (ev->device()->type() == QTouchDevice::TouchScreen) {
			touchEvent(ev);
			return true;
		}
	}

	return QWidget::event(e);
}

void OverviewInner::touchUpdateSpeed() {
	const uint64 nowTime = getms();
	if (_touchPrevPosValid) {
		const int elapsed = nowTime - _touchSpeedTime;
		if (elapsed) {
			const QPoint newPixelDiff = (_touchPos - _touchPrevPos);
			const QPoint pixelsPerSecond = newPixelDiff * (1000 / elapsed);

			// fingers are inacurates, we ignore small changes to avoid stopping the autoscroll because
			// of a small horizontal offset when scrolling vertically
			const int newSpeedY = (qAbs(pixelsPerSecond.y()) > FingerAccuracyThreshold) ? pixelsPerSecond.y() : 0;
			const int newSpeedX = (qAbs(pixelsPerSecond.x()) > FingerAccuracyThreshold) ? pixelsPerSecond.x() : 0;
			if (_touchScrollState == TouchScrollAuto) {
				const int oldSpeedY = _touchSpeed.y();
				const int oldSpeedX = _touchSpeed.x();
				if ((oldSpeedY <= 0 && newSpeedY <= 0) || ((oldSpeedY >= 0 && newSpeedY >= 0)
					&& (oldSpeedX <= 0 && newSpeedX <= 0)) || (oldSpeedX >= 0 && newSpeedX >= 0)) {
					_touchSpeed.setY(snap((oldSpeedY + (newSpeedY / 4)), -MaxScrollAccelerated, +MaxScrollAccelerated));
					_touchSpeed.setX(snap((oldSpeedX + (newSpeedX / 4)), -MaxScrollAccelerated, +MaxScrollAccelerated));
				} else {
					_touchSpeed = QPoint();
				}
			} else {
				// we average the speed to avoid strange effects with the last delta
				if (!_touchSpeed.isNull()) {
					_touchSpeed.setX(snap((_touchSpeed.x() / 4) + (newSpeedX * 3 / 4), -MaxScrollFlick, +MaxScrollFlick));
					_touchSpeed.setY(snap((_touchSpeed.y() / 4) + (newSpeedY * 3 / 4), -MaxScrollFlick, +MaxScrollFlick));
				} else {
					_touchSpeed = QPoint(newSpeedX, newSpeedY);
				}
			}
		}
	} else {
		_touchPrevPosValid = true;
	}
	_touchSpeedTime = nowTime;
	_touchPrevPos = _touchPos;
}

void OverviewInner::fixItemIndex(int32 &current, MsgId msgId) const {
	if (!msgId) {
		current = -1;
	} else if (_type == OverviewPhotos || _type == OverviewAudioDocuments) {
		int32 l = _hist->_overview[_type].size();
		if (current < 0 || current >= l || _hist->_overview[_type][current] != msgId) {
			current = -1;
			for (int32 i = 0; i < l; ++i) {
				if (_hist->_overview[_type][i] == msgId) {
					current = i;
					break;
				}
			}
		}
	} else {
		int32 l = _items.size();
		if (current < 0 || current >= l || _items[current].msgid != msgId) {
			current = -1;
			for (int32 i = 0; i < l; ++i) {
				if (_items[i].msgid == msgId) {
					current = i;
					break;
				}
			}
		}
	}
}

bool OverviewInner::itemHasPoint(MsgId msgId, int32 index, int32 x, int32 y) const {
	fixItemIndex(index, msgId);
	if (index < 0) return false;

	if (_type == OverviewPhotos) {
		if (x >= 0 && x < _vsize && y >= 0 && y < _vsize) {
			return true;
		}
	} else if (_type == OverviewAudioDocuments) {
		if (x >= _audioLeft && x < _audioLeft + _audioWidth && y >= 0 && y < _audioHeight) {
			return true;
		}
	} else {
		HistoryItem *item = App::histItemById(msgId);
		HistoryMedia *media = item ? item->getMedia(true) : 0;
		if (media) {
			int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
			bool out = item->out();
			int32 mw = media->maxWidth(), left = (out ? st::msgMargin.right() : st::msgMargin.left()) + (out && mw < w ? (w - mw) : 0);
			if (!out && _hist->peer->chat) {
				left += st::msgPhotoSkip;
			}
			return media->hasPoint(x - left, y - st::msgMargin.top(), item, w);
		}
	}
	return false;
}

int32 OverviewInner::itemHeight(MsgId msgId, int32 index) const {
	if (_type == OverviewPhotos) {
		return _vsize;
	} else if (_type == OverviewAudioDocuments) {
		return _audioHeight;
	}

	fixItemIndex(index, msgId);
	return (index < 0) ? 0 : (_items[index].y - (index > 0 ? _items[index - 1].y : 0));
}

void OverviewInner::moveToNextItem(MsgId &msgId, int32 &index, MsgId upTo, int32 delta) const {
	fixItemIndex(index, msgId);
	if (msgId == upTo || index < 0) {
		msgId = 0;
		index = -1;
		return;
	}

	index += delta;
	if (_type == OverviewPhotos || _type == OverviewAudioDocuments) {
		if (index < 0 || index >= _hist->_overview[_type].size()) {
			msgId = 0;
			index = -1;
		} else {
			msgId = _hist->_overview[_type][index];
		}
	} else {
		while (index >= 0 && index < _items.size() && !_items[index].msgid) {
			index += (delta > 0) ? 1 : -1;
		}
		if (index < 0 || index >= _items.size()) {
			msgId = 0;
			index = -1;
		} else {
			msgId = _items[index].msgid;
		}
	}
}

void OverviewInner::updateMsg(HistoryItem *item) {
	if (App::main() && item) {
		App::main()->msgUpdated(item->history()->peer->id, item);
	}
}

void OverviewInner::updateMsg(MsgId itemId, int32 itemIndex) {
	fixItemIndex(itemIndex, itemId);
	if (itemIndex >= 0) {
		if (_type == OverviewPhotos) {
			float64 w = (float64(_width - st::overviewPhotoSkip) / _photosInRow);
			int32 vsize = (_vsize + st::overviewPhotoSkip);
			int32 row = (_photosToAdd + itemIndex) / _photosInRow, col = (_photosToAdd + itemIndex) % _photosInRow;
			update(int32(col * w), _addToY + int32(row * vsize), qCeil(w), vsize);
		} else if (_type == OverviewAudioDocuments) {
			update(_audioLeft, _addToY + int32(itemIndex * _audioHeight), _audioWidth, _audioHeight);
		} else {
			HistoryItem *item = App::histItemById(itemId);
			HistoryMedia *media = item ? item->getMedia(true) : 0;
			int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
			if (media) update(0, _addToY + _height - _items[itemIndex].y, _width, media->countHeight(item, w) + st::msgMargin.top() + st::msgMargin.bottom());
		}
	}
}

void OverviewInner::touchResetSpeed() {
	_touchSpeed = QPoint();
	_touchPrevPosValid = false;
}

void OverviewInner::touchDeaccelerate(int32 elapsed) {
	int32 x = _touchSpeed.x();
	int32 y = _touchSpeed.y();
	_touchSpeed.setX((x == 0) ? x : (x > 0) ? qMax(0, x - elapsed) : qMin(0, x + elapsed));
	_touchSpeed.setY((y == 0) ? y : (y > 0) ? qMax(0, y - elapsed) : qMin(0, y + elapsed));
}

void OverviewInner::touchEvent(QTouchEvent *e) {
	const Qt::TouchPointStates &states(e->touchPointStates());
	if (e->type() == QEvent::TouchCancel) { // cancel
		if (!_touchInProgress) return;
		_touchInProgress = false;
		_touchSelectTimer.stop();
		_touchScroll = _touchSelect = false;
		_touchScrollState = TouchScrollManual;
		dragActionCancel();
		return;
	}

	if (!e->touchPoints().isEmpty()) {
		_touchPrevPos = _touchPos;
		_touchPos = e->touchPoints().cbegin()->screenPos().toPoint();
	}

	switch (e->type()) {
	case QEvent::TouchBegin:
		if (_touchInProgress) return;
		if (e->touchPoints().isEmpty()) return;

		_touchInProgress = true;
		if (_touchScrollState == TouchScrollAuto) {
			_touchScrollState = TouchScrollAcceleration;
			_touchWaitingAcceleration = true;
			_touchAccelerationTime = getms();
			touchUpdateSpeed();
			_touchStart = _touchPos;
		} else {
			_touchScroll = false;
			_touchSelectTimer.start(QApplication::startDragTime());
		}
		_touchSelect = false;
		_touchStart = _touchPrevPos = _touchPos;
		break;

	case QEvent::TouchUpdate:
		if (!_touchInProgress) return;
		if (_touchSelect) {
			dragActionUpdate(_touchPos);
		} else if (!_touchScroll && (_touchPos - _touchStart).manhattanLength() >= QApplication::startDragDistance()) {
			_touchSelectTimer.stop();
			_touchScroll = true;
			touchUpdateSpeed();
		}
		if (_touchScroll) {
			if (_touchScrollState == TouchScrollManual) {
				touchScrollUpdated(_touchPos);
			} else if (_touchScrollState == TouchScrollAcceleration) {
				touchUpdateSpeed();
				_touchAccelerationTime = getms();
				if (_touchSpeed.isNull()) {
					_touchScrollState = TouchScrollManual;
				}
			}
		}
		break;

	case QEvent::TouchEnd:
		if (!_touchInProgress) return;
		_touchInProgress = false;
		if (_touchSelect) {
			dragActionFinish(_touchPos, Qt::RightButton);
			QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, mapFromGlobal(_touchPos), _touchPos);
			showContextMenu(&contextMenu, true);
			_touchScroll = false;
		} else if (_touchScroll) {
			if (_touchScrollState == TouchScrollManual) {
				_touchScrollState = TouchScrollAuto;
				_touchPrevPosValid = false;
				_touchScrollTimer.start(15);
				_touchTime = getms();
			} else if (_touchScrollState == TouchScrollAuto) {
				_touchScrollState = TouchScrollManual;
				_touchScroll = false;
				touchResetSpeed();
			} else if (_touchScrollState == TouchScrollAcceleration) {
				_touchScrollState = TouchScrollAuto;
				_touchWaitingAcceleration = false;
				_touchPrevPosValid = false;
			}
		} else { // one short tap -- like mouse click
			dragActionStart(_touchPos);
			dragActionFinish(_touchPos);
		}
		_touchSelectTimer.stop();
		_touchSelect = false;
		break;
	}
}

void OverviewInner::dragActionUpdate(const QPoint &screenPos) {
	_dragPos = screenPos;
	onUpdateSelected();
}

void OverviewInner::dragActionStart(const QPoint &screenPos, Qt::MouseButton button) {
	dragActionUpdate(screenPos);
	if (button != Qt::LeftButton) return;

	if (textlnkDown() != textlnkOver()) {
		updateMsg(App::pressedLinkItem());
		textlnkDown(textlnkOver());
		App::pressedLinkItem(App::hoveredLinkItem());
		updateMsg(App::pressedLinkItem());
	}

	_dragAction = NoDrag;
	_dragItem = _mousedItem;
	_dragItemIndex = _mousedItemIndex;
	_dragStartPos = mapMouseToItem(mapFromGlobal(screenPos), _dragItem, _dragItemIndex);
	_dragWasInactive = App::wnd()->inactivePress();
	if (_dragWasInactive) App::wnd()->inactivePress(false);
	if (textlnkDown() && _selected.isEmpty()) {
		_dragAction = PrepareDrag;
	} else if (!_selected.isEmpty()) {
		if (_selected.cbegin().value() == FullItemSel) {
			if (_selected.constFind(_dragItem) != _selected.cend() && textlnkDown()) {
				_dragAction = PrepareDrag; // start items drag
			} else {
				_dragAction = PrepareSelect; // start items select
			}
		}
	}
	if (_dragAction == NoDrag && _dragItem) {
		bool afterDragSymbol = false , uponSymbol = false;
		uint16 symbol = 0;
		if (!_dragWasInactive) {
			if (textlnkDown()) {
				_dragSymbol = symbol;
				uint32 selStatus = (_dragSymbol << 16) | _dragSymbol;
				if (selStatus != FullItemSel && (_selected.isEmpty() || _selected.cbegin().value() != FullItemSel)) {
					if (!_selected.isEmpty()) {
						updateMsg(_selected.cbegin().key(), -1);
						_selected.clear();
					}
					_selected.insert(_dragItem, selStatus);
					_dragAction = Selecting;
					updateMsg(_dragItem, _dragItemIndex);
					_overview->updateTopBarSelection();
				} else {
					_dragAction = PrepareSelect;
				}
			} else {
				_dragAction = PrepareSelect; // start items select
			}
		}
	}

	if (!_dragItem) {
		_dragAction = NoDrag;
	} else if (_dragAction == NoDrag) {
		_dragItem = 0;
	}
}

void OverviewInner::dragActionCancel() {
	_dragItem = 0;
	_dragItemIndex = -1;
	_dragAction = NoDrag;
	_dragSelFrom = _dragSelTo = 0;
	_dragSelFromIndex = _dragSelToIndex = -1;
	_dragStartPos = QPoint(0, 0);
	_overview->noSelectingScroll();
}

void OverviewInner::dragActionFinish(const QPoint &screenPos, Qt::MouseButton button) {
	TextLinkPtr needClick;

	dragActionUpdate(screenPos);

	if (textlnkOver()) {
		if (textlnkDown() == textlnkOver() && _dragAction != Dragging && !_selMode) {
			needClick = textlnkDown();
		}
	}
	if (textlnkDown()) {
		updateMsg(App::pressedLinkItem());
		textlnkDown(TextLinkPtr());
		App::pressedLinkItem(0);
		if (!textlnkOver() && _cursor != style::cur_default) {
			_cursor = style::cur_default;
			setCursor(_cursor);
		}
	}
	if (needClick) {
		needClick->onClick(button);
		dragActionCancel();
		return;
	}
	if (_dragAction == PrepareSelect && !needClick && !_dragWasInactive && !_selected.isEmpty() && _selected.cbegin().value() == FullItemSel) {
		SelectedItems::iterator i = _selected.find(_dragItem);
		if (i == _selected.cend() && _dragItem > 0) {
			if (_selected.size() < MaxSelectedItems) {
				if (!_selected.isEmpty() && _selected.cbegin().value() != FullItemSel) {
					_selected.clear();
				}
				_selected.insert(_dragItem, FullItemSel);
			}
		} else {
			_selected.erase(i);
		}
		updateMsg(_dragItem, _dragItemIndex);
	} else if (_dragAction == PrepareDrag && !needClick && !_dragWasInactive && button != Qt::RightButton) {
		SelectedItems::iterator i = _selected.find(_dragItem);
		if (i != _selected.cend() && i.value() == FullItemSel) {
			_selected.erase(i);
			updateMsg(_dragItem, _dragItemIndex);
		} else if (i == _selected.cend() && _dragItem > 0 && !_selected.isEmpty() && _selected.cbegin().value() == FullItemSel) {
			if (_selected.size() < MaxSelectedItems) {
				_selected.insert(_dragItem, FullItemSel);
				updateMsg(_dragItem, _dragItemIndex);
			}
		} else {
			_selected.clear();
			parentWidget()->update();
		}
	} else if (_dragAction == Selecting) {
		if (_dragSelFrom && _dragSelTo) {
			applyDragSelection();
		} else if (!_selected.isEmpty() && !_dragWasInactive) {
			uint32 sel = _selected.cbegin().value();
			if (sel != FullItemSel && (sel & 0xFFFF) == ((sel >> 16) & 0xFFFF)) {
				_selected.clear();
				App::main()->activate();
			}
		}
	}
	_dragAction = NoDrag;
	_overview->noSelectingScroll();
	_overview->updateTopBarSelection();
}

void OverviewInner::onDragExec() {
	if (_dragAction != Dragging) return;

	bool uponSelected = false;
	if (_dragItem) {
		bool afterDragSymbol;
		uint16 symbol;
		if (!_selected.isEmpty() && _selected.cbegin().value() == FullItemSel) {
			uponSelected = _selected.contains(_dragItem);
		} else {
			uponSelected = false;
		}
	}
	QString sel;
	QList<QUrl> urls;
	bool forwardSelected = false;
	if (uponSelected) {
		forwardSelected = !_selected.isEmpty() && _selected.cbegin().value() == FullItemSel && cWideMode();
	} else if (textlnkDown()) {
		sel = textlnkDown()->encoded();
		if (!sel.isEmpty() && sel.at(0) != '/' && sel.at(0) != '@' && sel.at(0) != '#') {
//			urls.push_back(QUrl::fromEncoded(sel.toUtf8())); // Google Chrome crashes in Mac OS X O_o
		}
	}
	if (!sel.isEmpty() || forwardSelected) {
		updateDragSelection(0, -1, 0, -1, false);
		_overview->noSelectingScroll();

		QDrag *drag = new QDrag(App::wnd());
		QMimeData *mimeData = new QMimeData;

		if (!sel.isEmpty()) mimeData->setText(sel);
		if (!urls.isEmpty()) mimeData->setUrls(urls);
		if (forwardSelected) {
			mimeData->setData(qsl("application/x-td-forward-selected"), "1");
		}
		drag->setMimeData(mimeData);
		drag->exec(Qt::CopyAction);
		return;
	} else {
		HistoryItem *pressedLnkItem = App::pressedLinkItem(), *pressedItem = App::pressedItem();
		QLatin1String lnkType = (textlnkDown() && pressedLnkItem) ? textlnkDown()->type() : qstr("");
		bool lnkPhoto = (lnkType == qstr("PhotoLink")),
			lnkVideo = (lnkType == qstr("VideoOpenLink")),
			lnkAudio = (lnkType == qstr("AudioOpenLink")),
			lnkDocument = (lnkType == qstr("DocumentOpenLink"));
		if (lnkPhoto || lnkVideo || lnkAudio || lnkDocument) {
			QDrag *drag = new QDrag(App::wnd());
			QMimeData *mimeData = new QMimeData;

			mimeData->setData(qsl("application/x-td-forward-pressed-link"), "1");
			if (lnkDocument) {
				QString already = static_cast<DocumentOpenLink*>(textlnkDown().data())->document()->already(true);
				if (!already.isEmpty()) {
					QList<QUrl> urls;
					urls.push_back(QUrl::fromLocalFile(already));
					mimeData->setUrls(urls);
				}
			}

			drag->setMimeData(mimeData);
			drag->exec(Qt::CopyAction);
			return;
		}
	}
}

void OverviewInner::touchScrollUpdated(const QPoint &screenPos) {
	_touchPos = screenPos;
	_overview->touchScroll(_touchPos - _touchPrevPos);
	touchUpdateSpeed();
}

void OverviewInner::applyDragSelection() {
	if (_dragSelFromIndex < 0 || _dragSelToIndex < 0) return;

	if (!_selected.isEmpty() && _selected.cbegin().value() != FullItemSel) {
		_selected.clear();
	}
	if (_dragSelecting) {
		for (int32 i = _dragSelToIndex; i <= _dragSelFromIndex; ++i) {
			MsgId msgid = (_type == OverviewPhotos || _type == OverviewAudioDocuments) ? _hist->_overview[_type][i] : _items[i].msgid;
			if (!msgid) continue;

			SelectedItems::iterator j = _selected.find(msgid);
			if (msgid > 0) {
				if (j == _selected.cend()) {
					if (_selected.size() >= MaxSelectedItems) break;
					_selected.insert(msgid, FullItemSel);
				} else if (j.value() != FullItemSel) {
					*j = FullItemSel;
				}
			} else {
				if (j != _selected.cend()) {
					_selected.erase(j);
				}
			}
		}
	} else {
		for (int32 i = _dragSelToIndex; i <= _dragSelFromIndex; ++i) {
			MsgId msgid = (_type == OverviewPhotos || _type == OverviewAudioDocuments) ? _hist->_overview[_type][i] : _items[i].msgid;
			if (!msgid) continue;

			SelectedItems::iterator j = _selected.find(msgid);
			if (j != _selected.cend()) {
				_selected.erase(j);
			}
		}
	}
	_dragSelFrom = _dragSelTo = 0;
	_dragSelFromIndex = _dragSelToIndex = -1;
	_overview->updateTopBarSelection();
}

QPoint OverviewInner::mapMouseToItem(QPoint p, MsgId itemId, int32 itemIndex) {
	fixItemIndex(itemIndex, itemId);
	if (itemIndex < 0) return QPoint(0, 0);

	if (_type == OverviewPhotos) {
		int32 row = (_photosToAdd + itemIndex) / _photosInRow, col = (_photosToAdd + itemIndex) % _photosInRow;
		float64 w = (_width - st::overviewPhotoSkip) / float64(_photosInRow);
		p.setX(p.x() - int32(col * w) - st::overviewPhotoSkip);
		p.setY(p.y() - _addToY - row * (_vsize + st::overviewPhotoSkip) - st::overviewPhotoSkip);
	} else if (_type == OverviewAudioDocuments) {
		p.setY(p.y() - _addToY - itemIndex * _audioHeight);
	} else {
		p.setY(p.y() - _addToY - (_height - _items[itemIndex].y));
	}
	return p;
}

void OverviewInner::clear() {
	_cached.clear();
}

int32 OverviewInner::itemTop(MsgId msgId) const {
	if (_type == OverviewAudioDocuments) {
		int32 index = _hist->_overview[_type].indexOf(msgId);
		if (index >= 0) {
			return _addToY + int32(index * _audioHeight);
		}
	}
	return -1;
}

QPixmap OverviewInner::genPix(PhotoData *photo, int32 size) {
	size *= cIntRetinaFactor();
	QImage img = (photo->full->loaded() ? photo->full : (photo->medium->loaded() ? photo->medium : photo->thumb))->pix().toImage();
	if (!photo->full->loaded() && !photo->medium->loaded()) {
		img = imageBlur(img);
	}
	if (img.width() == img.height()) {
		if (img.width() != size) {
			img = img.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
		}
	} else if (img.width() > img.height()) {
        img = img.copy((img.width() - img.height()) / 2, 0, img.height(), img.height()).scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	} else {
        img = img.copy(0, (img.height() - img.width()) / 2, img.width(), img.width()).scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	}
//	imageRound(img);
	img.setDevicePixelRatio(cRetinaFactor());
	photo->forget();
	return QPixmap::fromImage(img, Qt::ColorOnly);
}

void OverviewInner::paintEvent(QPaintEvent *e) {
	QPainter p(this);

	QRect r(e->rect());
	p.setClipRect(r);

	if (_hist->_overview[_type].isEmpty()) {
		QPoint dogPos((_width - st::msgDogImg.pxWidth()) / 2, ((height() - st::msgDogImg.pxHeight()) * 4) / 9);
		p.drawPixmap(dogPos, *cChatDogImage());
		return;
	}

	int32 selfrom = -1, selto = -1;
	if (_dragSelFromIndex >= 0 && _dragSelToIndex >= 0) {
		selfrom = _dragSelToIndex;
		selto = _dragSelFromIndex;
	}

	SelectedItems::const_iterator selEnd = _selected.cend();
	bool hasSel = !_selected.isEmpty();

	if (_type == OverviewPhotos) {
		int32 rowFrom = int32(r.top() - _addToY - st::overviewPhotoSkip) / int32(_vsize + st::overviewPhotoSkip);
		int32 rowTo = int32(r.bottom() - _addToY - st::overviewPhotoSkip) / int32(_vsize + st::overviewPhotoSkip) + 1;
		History::MediaOverview &overview(_hist->_overview[_type]);
		int32 count = overview.size();
		float64 w = float64(_width - st::overviewPhotoSkip) / _photosInRow;
		for (int32 row = rowFrom; row < rowTo; ++row) {
			if (row * _photosInRow >= _photosToAdd + count) break;
			for (int32 i = 0; i < _photosInRow; ++i) {
				int32 index = row * _photosInRow + i - _photosToAdd;
				if (index < 0) continue;
				if (index >= count) break;

				HistoryItem *item = App::histItemById(overview[index]);
				HistoryMedia *m = item ? item->getMedia(true) : 0;
				if (!m) continue;

				switch (m->type()) {
				case MediaTypePhoto: {
					PhotoData *photo = static_cast<HistoryPhoto*>(m)->photo();
					bool quality = photo->full->loaded();
					if (!quality) {
						if (photo->thumb->loaded()) {
							photo->medium->load(false, false);
							quality = photo->medium->loaded();
						} else {
							photo->thumb->load();
						}
					}
					CachedSizes::iterator it = _cached.find(photo);
					if (it == _cached.cend()) {
						CachedSize size;
						size.medium = quality;
						size.vsize = _vsize;
						size.pix = genPix(photo, _vsize);
						it = _cached.insert(photo, size);
					} else if (it->medium != quality || it->vsize != _vsize) {
						it->medium = quality;
						it->vsize = _vsize;
						it->pix = genPix(photo, _vsize);
					}
					QPoint pos(int32(i * w + st::overviewPhotoSkip), _addToY + row * (_vsize + st::overviewPhotoSkip) + st::overviewPhotoSkip);
					p.drawPixmap(pos, it->pix);
					if (!quality) {
						uint64 dt = itemAnimations().animate(item, getms());
						int32 cnt = int32(st::photoLoaderCnt), period = int32(st::photoLoaderPeriod), t = dt % period, delta = int32(st::photoLoaderDelta);

						int32 x = pos.x() + (_vsize - st::overviewLoader.width()) / 2, y = pos.y() + (_vsize - st::overviewLoader.height()) / 2;
						p.fillRect(x, y, st::overviewLoader.width(), st::overviewLoader.height(), st::photoLoaderBg->b);
						x += (st::overviewLoader.width() - cnt * st::overviewLoaderPoint.width() - (cnt - 1) * st::overviewLoaderSkip) / 2;
						y += (st::overviewLoader.height() - st::overviewLoaderPoint.height()) / 2;
						QColor c(st::white->c);
						QBrush b(c);
						for (int32 i = 0; i < cnt; ++i) {
							t -= delta;
							while (t < 0) t += period;

							float64 alpha = (t >= st::photoLoaderDuration1 + st::photoLoaderDuration2) ? 0 : ((t > st::photoLoaderDuration1 ? ((st::photoLoaderDuration1 + st::photoLoaderDuration2 - t) / st::photoLoaderDuration2) : (t / st::photoLoaderDuration1)));
							c.setAlphaF(st::photoLoaderAlphaMin + alpha * (1 - st::photoLoaderAlphaMin));
							b.setColor(c);
							p.fillRect(x + i * (st::overviewLoaderPoint.width() + st::overviewLoaderSkip), y, st::overviewLoaderPoint.width(), st::overviewLoaderPoint.height(), b);
						}
					}

					uint32 sel = 0;
					if (index >= selfrom && index <= selto) {
						sel = (_dragSelecting && item->id > 0) ? FullItemSel : 0;
					} else if (hasSel) {
						SelectedItems::const_iterator i = _selected.constFind(item->id);
						if (i != selEnd) {
							sel = i.value();
						}
					}
					if (sel == FullItemSel) {
						p.fillRect(QRect(pos.x(), pos.y(), _vsize, _vsize), st::overviewPhotoSelectOverlay->b);
						p.drawPixmap(QPoint(pos.x() + _vsize - st::overviewPhotoChecked.pxWidth(), pos.y() + _vsize - st::overviewPhotoChecked.pxHeight()), App::sprite(), st::overviewPhotoChecked);
					} else if (_selMode/* || (selfrom < count && selfrom <= selto && 0 <= selto)*/) {
						p.drawPixmap(QPoint(pos.x() + _vsize - st::overviewPhotoChecked.pxWidth(), pos.y() + _vsize - st::overviewPhotoChecked.pxHeight()), App::sprite(), st::overviewPhotoCheck);
					}
				} break;
				}
			}
		}
	} else if (_type == OverviewAudioDocuments) {
		int32 from = int32(r.top() - _addToY) / int32(_audioHeight);
		int32 to = int32(r.bottom() - _addToY) / int32(_audioHeight) + 1;
		History::MediaOverview &overview(_hist->_overview[_type]);
		int32 count = overview.size();
		p.translate(_audioLeft, _addToY + from * _audioHeight);
		for (int32 index = from; index < to; ++index) {
			if (index >= count) break;

			HistoryItem *item = App::histItemById(overview[index]);
			HistoryMedia *m = item ? item->getMedia(true) : 0;
			if (!m || m->type() != MediaTypeDocument) continue;

			uint32 sel = 0;
			if (index >= selfrom && index <= selto) {
				sel = (_dragSelecting && item->id > 0) ? FullItemSel : 0;
			} else if (hasSel) {
				SelectedItems::const_iterator i = _selected.constFind(item->id);
				if (i != selEnd) {
					sel = i.value();
				}
			}

			static_cast<HistoryDocument*>(m)->drawInPlaylist(p, item, (sel == FullItemSel), ((_menu ? (App::contextItem() ? App::contextItem()->id : 0) : _selectedMsgId) == item->id), _audioWidth);
			p.translate(0, _audioHeight);
		}
	} else {
		p.translate(0, st::msgMargin.top() + _addToY);
		int32 y = 0, w = _width - st::msgMargin.left() - st::msgMargin.right();
		for (int32 i = _items.size(); i > 0;) {
			--i;
			if (!i || (_addToY + _height - _items[i - 1].y > r.top())) {
				int32 curY = _height - _items[i].y;
				if (_addToY + curY >= r.bottom()) break;

				p.translate(0, curY - y);
				if (_items[i].msgid) { // draw item
					HistoryItem *item = App::histItemById(_items[i].msgid);
					HistoryMedia *media = item ? item->getMedia(true) : 0;
					if (media) {
						bool out = item->out();
						int32 mw = media->maxWidth(), left = (out ? st::msgMargin.right() : st::msgMargin.left()) + (out && mw < w ? (w - mw) : 0);
						if (!out && _hist->peer->chat) {
							p.drawPixmap(left, media->countHeight(item, w) - st::msgPhotoSize, item->from()->photo->pixRounded(st::msgPhotoSize));
							left += st::msgPhotoSkip;
						}

						uint32 sel = 0;
						if (i >= selfrom && i <= selto) {
							sel = (_dragSelecting && item->id > 0) ? FullItemSel : 0;
						} else if (hasSel) {
							SelectedItems::const_iterator i = _selected.constFind(item->id);
							if (i != selEnd) {
								sel = i.value();
							}
						}

						p.save();
						p.translate(left, 0);
						media->draw(p, item, (sel == FullItemSel), w);
						p.restore();
					}
				} else {
					QString str = langDayOfMonth(_items[i].date);

					int32 left = st::msgServiceMargin.left(), width = _width - st::msgServiceMargin.left() - st::msgServiceMargin.left(), height = st::msgServiceFont->height + st::msgServicePadding.top() + st::msgServicePadding.bottom();
					if (width < 1) return;

					int32 strwidth = st::msgServiceFont->m.width(str) + st::msgServicePadding.left() + st::msgServicePadding.right();

					QRect trect(QRect(left, st::msgServiceMargin.top(), width, height).marginsAdded(-st::msgServicePadding));
					left += (width - strwidth) / 2;
					width = strwidth;

					QRect r(left, st::msgServiceMargin.top(), width, height);
					App::roundRect(p, r, App::msgServiceBg(), ServiceCorners);

					p.setBrush(Qt::NoBrush);
					p.setPen(st::msgServiceColor->p);
					p.setFont(st::msgServiceFont->f);
					p.drawText(r.x() + st::msgServicePadding.left(), r.y() + st::msgServicePadding.top() + st::msgServiceFont->ascent, str);
				}
				y = curY;
			}
		}
	}
}

void OverviewInner::mouseMoveEvent(QMouseEvent *e) {
	if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton)) && (textlnkDown() || _dragAction != NoDrag)) {
		mouseReleaseEvent(e);
	}
	dragActionUpdate(e->globalPos());
}

void OverviewInner::onUpdateSelected() {
	if (isHidden()) return;

	QPoint mousePos(mapFromGlobal(_dragPos));
	QPoint m(_overview->clampMousePosition(mousePos));

	TextLinkPtr lnk;
	HistoryItem *item = 0;
	int32 index = -1;
	int32 newsel = 0;
	if (_type == OverviewPhotos) {
		float64 w = (float64(_width - st::overviewPhotoSkip) / _photosInRow);
		int32 inRow = int32((m.x() - (st::overviewPhotoSkip / 2)) / w), vsize = (_vsize + st::overviewPhotoSkip);
		int32 row = int32((m.y() - _addToY - (st::overviewPhotoSkip / 2)) / vsize);
		if (inRow < 0) inRow = 0;
		if (row < 0) row = 0;
		bool upon = true;

		int32 i = row * _photosInRow + inRow - _photosToAdd, count = _hist->_overview[_type].size();
		if (!count) return;

		if (i < 0) {
			i = 0;
			upon = false;
		}
		if (i >= count) {
			i = count - 1;
			upon = false;
		}
		MsgId msgid = _hist->_overview[_type][i];
		HistoryItem *histItem = App::histItemById(msgid);
		if (histItem) {
			item = histItem;
			index = i;
			if (upon && m.x() >= inRow * w + st::overviewPhotoSkip && m.x() < inRow * w + st::overviewPhotoSkip + _vsize) {
				if (m.y() >= _addToY + row * vsize + st::overviewPhotoSkip && m.y() < _addToY + (row + 1) * vsize + st::overviewPhotoSkip) {
					HistoryMedia *media = item->getMedia(true);
					if (media && media->type() == MediaTypePhoto) {
						lnk = static_cast<HistoryPhoto*>(media)->lnk();
					}
				}
			}
		} else {
			return;
		}
	} else if (_type == OverviewAudioDocuments) {
		int32 i = int32((m.y() - _addToY) / _audioHeight), count = _hist->_overview[_type].size();
		if (!count) return;

		bool upon = true;
		if (m.y() < _addToY) {
			i = 0;
			upon = false;
		}
		if (i >= count) {
			i = count - 1;
			upon = false;
		}
		MsgId msgid = _hist->_overview[_type][i];
		HistoryItem *histItem = App::histItemById(msgid);
		if (histItem) {
			item = histItem;
			index = i;
			if (upon && m.x() >= _audioLeft && m.x() < _audioLeft + _audioWidth) {
				HistoryMedia *media = item->getMedia(true);
				if (media && media->type() == MediaTypeDocument) {
					lnk = static_cast<HistoryDocument*>(media)->linkInPlaylist();
					newsel = item->id;
				}
			}
			if (newsel != _selectedMsgId) {
				updateMsg(App::histItemById(_selectedMsgId));
				_selectedMsgId = newsel;
				updateMsg(item);
			}
		} else {
			if (newsel != _selectedMsgId) {
				updateMsg(App::histItemById(_selectedMsgId));
				_selectedMsgId = newsel;
			}
			return;
		}
	} else {
		int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
		if (_items.isEmpty()) return;

		for (int32 i = _items.size(); i > 0;) {
			--i;
			if (!i || (_addToY + _height - _items[i - 1].y > m.y())) {
				int32 y = _addToY + _height - _items[i].y;
				if (item) break;

				if (!_items[i].msgid) { // day item
					int32 h = itemHeight(_items[i].msgid, i);
					if (i > 0 && ((y + h / 2) < m.y() || i == _items.size() - 1)) {
						--i;
						if (!_items[i].msgid) break; // wtf
						y = _addToY + _height - _items[i].y;
					} else if (i < _items.size() - 1 && ((y + h / 2) >= m.y() || !i)) {
						++i;
						if (!_items[i].msgid) break; // wtf
						y = _addToY + _height - _items[i].y;
					} else {
						break; // wtf
					}
				}

				HistoryItem *histItem = App::histItemById(_items[i].msgid);
				if (histItem) {
					item = histItem;
					index = i;
					HistoryMedia *media = item->getMedia(true);
					if (media) {
						bool out = item->out();
						int32 mw = media->maxWidth(), left = (out ? st::msgMargin.right() : st::msgMargin.left()) + (out && mw < w ? (w - mw) : 0);
						if (!out && _hist->peer->chat) {
							if (QRect(left, y + st::msgMargin.top() + media->countHeight(item, w) - st::msgPhotoSize, st::msgPhotoSize, st::msgPhotoSize).contains(m)) {
								lnk = item->from()->lnk;
							}
							left += st::msgPhotoSkip;
						}
						HistoryCursorState cursorState = HistoryDefaultCursorState;
						TextLinkPtr link;
						media->getState(link, cursorState, m.x() - left, m.y() - y - st::msgMargin.top(), item, w);
						if (link) lnk = link;
					}
				} else {
					return;
				}
			}
		}
	}

	_mousedItem = item ? item->id : 0;
	_mousedItemIndex = index;
	m = mapMouseToItem(m, _mousedItem, _mousedItemIndex);

	Qt::CursorShape cur = style::cur_default;
	bool lnkChanged = false;
	if (lnk != textlnkOver()) {
		lnkChanged = true;
		updateMsg(App::hoveredLinkItem());
		textlnkOver(lnk);
		App::hoveredLinkItem(lnk ? item : 0);
		updateMsg(App::hoveredLinkItem());
	} else {
		App::mousedItem(item);
	}

	fixItemIndex(_dragItemIndex, _dragItem);
	fixItemIndex(_mousedItemIndex, _mousedItem);
	if (_dragAction == NoDrag) {
		if (lnk) {
			cur = style::cur_pointer;
		}
	} else {
		if (_dragItemIndex < 0 || _mousedItemIndex < 0) {
			_dragAction = NoDrag;
			return;
		}
		if (_mousedItem != _dragItem || (m - _dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
			if (_dragAction == PrepareDrag) {
				_dragAction = Dragging;
				QTimer::singleShot(1, this, SLOT(onDragExec()));
			} else if (_dragAction == PrepareSelect) {
				_dragAction = Selecting;
			}
		}
		cur = textlnkDown() ? style::cur_pointer : style::cur_default;
		if (_dragAction == Selecting) {
			if (_mousedItem == _dragItem && lnk && !_selected.isEmpty() && _selected.cbegin().value() != FullItemSel) {
				bool afterSymbol = false, uponSymbol = false;
				uint16 second = 0;
				_selected[_dragItem] = 0;
				updateDragSelection(0, -1, 0, -1, false);
			} else {
				bool selectingDown = ((_type == OverviewPhotos || _type == OverviewAudioDocuments) ? (_mousedItemIndex > _dragItemIndex) : (_mousedItemIndex < _dragItemIndex)) || (_mousedItemIndex == _dragItemIndex && (_type == OverviewPhotos ? (_dragStartPos.x() < m.x()) : (_dragStartPos.y() < m.y())));
				MsgId dragSelFrom = _dragItem, dragSelTo = _mousedItem;
				int32 dragSelFromIndex = _dragItemIndex, dragSelToIndex = _mousedItemIndex;
				if (!itemHasPoint(dragSelFrom, dragSelFromIndex, _dragStartPos.x(), _dragStartPos.y())) { // maybe exclude dragSelFrom
					if (selectingDown) {
						if (_type == OverviewPhotos) {
							if (_dragStartPos.x() >= _vsize || ((_mousedItem == dragSelFrom) && (m.x() < _dragStartPos.x() + QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, 1);
							}
						} else if (_type == OverviewAudioDocuments) {
							if (_dragStartPos.y() >= itemHeight(dragSelFrom, dragSelFromIndex) || ((_mousedItem == dragSelFrom) && (m.y() < _dragStartPos.y() + QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, 1);
							}
						} else {
							if (_dragStartPos.y() >= (itemHeight(dragSelFrom, dragSelFromIndex) - st::msgMargin.bottom()) || ((_mousedItem == dragSelFrom) && (m.y() < _dragStartPos.y() + QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, -1);
							}
						}
					} else {
						if (_type == OverviewPhotos) {
							if (_dragStartPos.x() < 0 || ((_mousedItem == dragSelFrom) && (m.x() >= _dragStartPos.x() - QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, -1);
							}
						} else if (_type == OverviewAudioDocuments) {
							if (_dragStartPos.y() < 0 || ((_mousedItem == dragSelFrom) && (m.y() >= _dragStartPos.y() - QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, -1);
							}
						} else {
							if (_dragStartPos.y() < st::msgMargin.top() || ((_mousedItem == dragSelFrom) && (m.y() >= _dragStartPos.y() - QApplication::startDragDistance()))) {
								moveToNextItem(dragSelFrom, dragSelFromIndex, dragSelTo, 1);
							}
						}
					}
				}
				if (_dragItem != _mousedItem) { // maybe exclude dragSelTo
					if (selectingDown) {
						if (_type == OverviewPhotos) {
							if (m.x() < 0) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, -1);
							}
						} else if (_type == OverviewAudioDocuments) {
							if (m.y() < 0) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, 1);
							}
						} else {
							if (m.y() < st::msgMargin.top()) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, 1);
							}
						}
					} else {
						if (_type == OverviewPhotos) {
							if (m.x() >= _vsize) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, 1);
							}
						} else if (_type == OverviewAudioDocuments) {
							if (m.y() >= itemHeight(dragSelTo, dragSelToIndex)) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, 1);
							}
						} else {
							if (m.y() >= itemHeight(dragSelTo, dragSelToIndex) - st::msgMargin.bottom()) {
								moveToNextItem(dragSelTo, dragSelToIndex, dragSelFrom, -1);
							}
						}
					}
				}
				bool dragSelecting = false;
				MsgId dragFirstAffected = dragSelFrom;
				int32 dragFirstAffectedIndex = dragSelFromIndex;
				while (dragFirstAffectedIndex >= 0 && dragFirstAffected <= 0) {
					moveToNextItem(dragFirstAffected, dragFirstAffectedIndex, dragSelTo, ((selectingDown && (_type == OverviewPhotos || _type == OverviewAudioDocuments)) || (!selectingDown && (_type != OverviewPhotos && _type != OverviewAudioDocuments))) ? -1 : 1);
				}
				if (dragFirstAffectedIndex >= 0) {
					SelectedItems::const_iterator i = _selected.constFind(dragFirstAffected);
					dragSelecting = (i == _selected.cend() || i.value() != FullItemSel);
				}
				updateDragSelection(dragSelFrom, dragSelFromIndex, dragSelTo, dragSelToIndex, dragSelecting);
			}
		} else if (_dragAction == Dragging) {
		}

		if (textlnkDown()) {
			cur = style::cur_pointer;
		} else if (_dragAction == Selecting && !_selected.isEmpty() && _selected.cbegin().value() != FullItemSel) {
			if (!_dragSelFrom || !_dragSelTo) {
				cur = style::cur_text;
			}
		}
	}
	if (_dragAction == Selecting) {
		_overview->checkSelectingScroll(mousePos);
	} else {
		updateDragSelection(0, -1, 0, -1, false);
		_overview->noSelectingScroll();
	}

	if (lnkChanged || cur != _cursor) {
		setCursor(_cursor = cur);
	}
}

void OverviewInner::updateDragSelection(MsgId dragSelFrom, int32 dragSelFromIndex, MsgId dragSelTo, int32 dragSelToIndex, bool dragSelecting) {
	if (_dragSelFrom != dragSelFrom || _dragSelFromIndex != dragSelFromIndex || _dragSelTo != dragSelTo || _dragSelToIndex != dragSelToIndex || _dragSelecting != dragSelecting) {
		_dragSelFrom = dragSelFrom;
		_dragSelFromIndex = dragSelFromIndex;
		_dragSelTo = dragSelTo;
		_dragSelToIndex = dragSelToIndex;
		if (_dragSelFromIndex >= 0 && _dragSelToIndex >= 0 && _dragSelFromIndex < _dragSelToIndex) {
			qSwap(_dragSelFrom, _dragSelTo);
			qSwap(_dragSelFromIndex, _dragSelToIndex);
		}
		_dragSelecting = dragSelecting;
		parentWidget()->update();
	}
}

void OverviewInner::mousePressEvent(QMouseEvent *e) {
	if (_menu) {
		e->accept();
		return; // ignore mouse press, that was hiding context menu
	}
	dragActionStart(e->globalPos(), e->button());
}

void OverviewInner::mouseReleaseEvent(QMouseEvent *e) {
	dragActionFinish(e->globalPos(), e->button());
	if (!rect().contains(e->pos())) {
		leaveEvent(e);
	}
}

void OverviewInner::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) {
		if (_selected.isEmpty() || e->key() == Qt::Key_Back) {
			App::main()->showBackFromStack();
		} else {
			_overview->onClearSelected();
		}
	}
}

void OverviewInner::enterEvent(QEvent *e) {
	return QWidget::enterEvent(e);
}

void OverviewInner::leaveEvent(QEvent *e) {
	if (_selectedMsgId > 0) {
		updateMsg(App::histItemById(_selectedMsgId));
		_selectedMsgId = 0;
	}
	if (textlnkOver()) {
		updateMsg(App::hoveredLinkItem());
		textlnkOver(TextLinkPtr());
		App::hoveredLinkItem(0);
		if (!textlnkDown() && _cursor != style::cur_default) {
			_cursor = style::cur_default;
			setCursor(_cursor);
		}
	}
	return QWidget::leaveEvent(e);
}

void OverviewInner::resizeEvent(QResizeEvent *e) {
	_width = width();
	_audioWidth = qMin(_width - st::profilePadding.left() - st::profilePadding.right(), int(st::profileMaxWidth));
	_audioLeft = (_width - _audioWidth) / 2;
	showAll(true);
	onUpdateSelected();
	update();
}

void OverviewInner::showContextMenu(QContextMenuEvent *e, bool showFromTouch) {
	if (_menu) {
		_menu->deleteLater();
		_menu = 0;
		updateMsg(App::contextItem());
		if (_selectedMsgId > 0) updateMsg(App::histItemById(_selectedMsgId));
	}
	if (e->reason() == QContextMenuEvent::Mouse) {
		dragActionUpdate(e->globalPos());
	}

	// -2 - has full selected items, but not over, 0 - no selection, 2 - over full selected items
	int32 isUponSelected = 0, hasSelected = 0;
	if (!_selected.isEmpty()) {
		isUponSelected = -1;
		if (_selected.cbegin().value() == FullItemSel) {
			hasSelected = 2;
			if (App::hoveredLinkItem() && _selected.constFind(App::hoveredLinkItem()->id) != _selected.cend()) {
				isUponSelected = 2;
			} else {
				isUponSelected = -2;
			}
		}
	}
	if (showFromTouch && hasSelected && isUponSelected < hasSelected) {
		isUponSelected = hasSelected;
	}

	_contextMenuLnk = textlnkOver();
	PhotoLink *lnkPhoto = dynamic_cast<PhotoLink*>(_contextMenuLnk.data());
	VideoLink *lnkVideo = dynamic_cast<VideoLink*>(_contextMenuLnk.data());
	AudioLink *lnkAudio = dynamic_cast<AudioLink*>(_contextMenuLnk.data());
	DocumentLink *lnkDocument = dynamic_cast<DocumentLink*>(_contextMenuLnk.data());
	if (lnkPhoto || lnkVideo || lnkAudio || lnkDocument) {
		_menu = new ContextMenu(_overview);
		if (App::hoveredLinkItem()) {
			_menu->addAction(lang(lng_context_to_msg), this, SLOT(goToMessage()))->setEnabled(true);
		}
		if (lnkPhoto) {
			_menu->addAction(lang(lng_context_open_image), this, SLOT(openContextUrl()))->setEnabled(true);
		} else {
			if ((lnkVideo && lnkVideo->video()->loader) || (lnkAudio && lnkAudio->audio()->loader) || (lnkDocument && lnkDocument->document()->loader)) {
				_menu->addAction(lang(lng_context_cancel_download), this, SLOT(cancelContextDownload()))->setEnabled(true);
			} else {
				if ((lnkVideo && !lnkVideo->video()->already(true).isEmpty()) || (lnkAudio && !lnkAudio->audio()->already(true).isEmpty()) || (lnkDocument && !lnkDocument->document()->already(true).isEmpty())) {
					_menu->addAction(lang(cPlatform() == dbipMac ? lng_context_show_in_finder : lng_context_show_in_folder), this, SLOT(showContextInFolder()))->setEnabled(true);
				}
				_menu->addAction(lang(lnkVideo ? lng_context_open_video : (lnkAudio ? lng_context_open_audio : lng_context_open_file)), this, SLOT(openContextFile()))->setEnabled(true);
				_menu->addAction(lang(lnkVideo ? lng_context_save_video : (lnkAudio ? lng_context_save_audio : lng_context_save_file)), this, SLOT(saveContextFile()))->setEnabled(true);
			}
		}
		if (isUponSelected > 1) {
			_menu->addAction(lang(lng_context_forward_selected), _overview, SLOT(onForwardSelected()));
			_menu->addAction(lang(lng_context_delete_selected), _overview, SLOT(onDeleteSelected()));
			_menu->addAction(lang(lng_context_clear_selection), _overview, SLOT(onClearSelected()));
		} else if (App::hoveredLinkItem()) {
			if (isUponSelected != -2) {
				if (dynamic_cast<HistoryMessage*>(App::hoveredLinkItem())) {
					_menu->addAction(lang(lng_context_forward_msg), this, SLOT(forwardMessage()))->setEnabled(true);
				}
				_menu->addAction(lang(lng_context_delete_msg), this, SLOT(deleteMessage()))->setEnabled(true);
			}
			_menu->addAction(lang(lng_context_select_msg), this, SLOT(selectMessage()))->setEnabled(true);
		}
		App::contextItem(App::hoveredLinkItem());
		updateMsg(App::contextItem());
		if (_selectedMsgId > 0) updateMsg(App::histItemById(_selectedMsgId));
	} else if (App::mousedItem() && App::mousedItem()->id == _mousedItem) {
		_menu = new ContextMenu(_overview);
		_menu->addAction(lang(lng_context_to_msg), this, SLOT(goToMessage()))->setEnabled(true);
		if (isUponSelected > 1) {
			_menu->addAction(lang(lng_context_forward_selected), _overview, SLOT(onForwardSelected()));
			_menu->addAction(lang(lng_context_delete_selected), _overview, SLOT(onDeleteSelected()));
			_menu->addAction(lang(lng_context_clear_selection), _overview, SLOT(onClearSelected()));
		} else {
			if (isUponSelected != -2) {
				if (dynamic_cast<HistoryMessage*>(App::mousedItem())) {
					_menu->addAction(lang(lng_context_forward_msg), this, SLOT(forwardMessage()))->setEnabled(true);
				}
				_menu->addAction(lang(lng_context_delete_msg), this, SLOT(deleteMessage()))->setEnabled(true);
			}
			_menu->addAction(lang(lng_context_select_msg), this, SLOT(selectMessage()))->setEnabled(true);
		}
		App::contextItem(App::mousedItem());
		updateMsg(App::contextItem());
		if (_selectedMsgId > 0) updateMsg(App::histItemById(_selectedMsgId));
	}
	if (_menu) {
		_menu->deleteOnHide();
		connect(_menu, SIGNAL(destroyed(QObject*)), this, SLOT(onMenuDestroy(QObject*)));
		_menu->popup(e->globalPos());
		e->accept();
	}
}

int32 OverviewInner::resizeToWidth(int32 nwidth, int32 scrollTop, int32 minHeight) {
	if (width() == nwidth && minHeight == _minHeight) return scrollTop;
	_minHeight = minHeight;
	_addToY = (_type == OverviewAudioDocuments) ? st::playlistPadding : ((_height < _minHeight) ? (_minHeight - _height) : 0);
	if (_type == OverviewPhotos && _resizeIndex < 0) {
		_resizeIndex = _photosInRow * ((scrollTop + minHeight) / int32(_vsize + st::overviewPhotoSkip)) + _photosInRow - 1;
		_resizeSkip = (scrollTop + minHeight) - ((scrollTop + minHeight) / int32(_vsize + st::overviewPhotoSkip)) * int32(_vsize + st::overviewPhotoSkip);
	}
	resize(nwidth, height() > _minHeight ? height() : _minHeight);
	showAll();
	if (_type == OverviewPhotos) {
        int32 newRow = _resizeIndex / _photosInRow;
        return newRow * int32(_vsize + st::overviewPhotoSkip) + _resizeSkip - minHeight;
    }
    return scrollTop;
}

void OverviewInner::dropResizeIndex() {
	_resizeIndex = -1;
}

PeerData *OverviewInner::peer() const {
	return _peer;
}

MediaOverviewType OverviewInner::type() const {
	return _type;
}

void OverviewInner::switchType(MediaOverviewType type) {
	if (_type != type) {
		_selected.clear();
		_dragItemIndex = _mousedItemIndex = _dragSelFromIndex = _dragSelToIndex = -1;
		_dragItem = _mousedItem = _dragSelFrom = _dragSelTo = 0;
		_items.clear();
		_cached.clear();
		_type = type;
	}
	mediaOverviewUpdated();
	if (App::wnd()) App::wnd()->update();
}

void OverviewInner::setSelectMode(bool enabled) {
	_selMode = enabled;
}

void OverviewInner::openContextUrl() {
	HistoryItem *was = App::hoveredLinkItem();
	App::hoveredLinkItem(App::contextItem());
	_contextMenuLnk->onClick(Qt::LeftButton);
	App::hoveredLinkItem(was);
}

void OverviewInner::goToMessage() {
	HistoryItem *item = App::contextItem();
	if (!item) return;

	App::main()->showPeerHistory(item->history()->peer->id, item->id);
}

void OverviewInner::forwardMessage() {
	HistoryItem *item = App::contextItem();
	if (!item || item->itemType() != HistoryItem::MsgType) return;

	App::main()->forwardLayer();
}

void OverviewInner::deleteMessage() {
	HistoryItem *item = App::contextItem();
	if (!item || item->itemType() != HistoryItem::MsgType) return;

	HistoryMessage *msg = dynamic_cast<HistoryMessage*>(item);
	App::main()->deleteLayer((msg && msg->uploading()) ? -2 : -1);
}

void OverviewInner::selectMessage() {
	HistoryItem *item = App::contextItem();
	if (!item || item->itemType() != HistoryItem::MsgType) return;

	if (!_selected.isEmpty() && _selected.cbegin().value() != FullItemSel) {
		_selected.clear();
	} else if (_selected.size() == MaxSelectedItems && _selected.constFind(item->id) == _selected.cend()) {
		return;
	}
	_selected.insert(item->id, FullItemSel);
	_overview->updateTopBarSelection();
	_overview->update();
}

void OverviewInner::cancelContextDownload() {
	VideoLink *lnkVideo = dynamic_cast<VideoLink*>(_contextMenuLnk.data());
	AudioLink *lnkAudio = dynamic_cast<AudioLink*>(_contextMenuLnk.data());
	DocumentLink *lnkDocument = dynamic_cast<DocumentLink*>(_contextMenuLnk.data());
	mtpFileLoader *loader = lnkVideo ? lnkVideo->video()->loader : (lnkAudio ? lnkAudio->audio()->loader : (lnkDocument ? lnkDocument->document()->loader : 0));
	if (loader) loader->cancel();
}

void OverviewInner::showContextInFolder() {
	VideoLink *lnkVideo = dynamic_cast<VideoLink*>(_contextMenuLnk.data());
	AudioLink *lnkAudio = dynamic_cast<AudioLink*>(_contextMenuLnk.data());
	DocumentLink *lnkDocument = dynamic_cast<DocumentLink*>(_contextMenuLnk.data());
	QString already = lnkVideo ? lnkVideo->video()->already(true) : (lnkAudio ? lnkAudio->audio()->already(true) : (lnkDocument ? lnkDocument->document()->already(true) : QString()));
	if (!already.isEmpty()) psShowInFolder(already);
}

void OverviewInner::saveContextFile() {
	VideoLink *lnkVideo = dynamic_cast<VideoLink*>(_contextMenuLnk.data());
	AudioLink *lnkAudio = dynamic_cast<AudioLink*>(_contextMenuLnk.data());
	DocumentLink *lnkDocument = dynamic_cast<DocumentLink*>(_contextMenuLnk.data());
	if (lnkVideo) VideoSaveLink::doSave(lnkVideo->video(), true);
	if (lnkAudio) AudioSaveLink::doSave(lnkAudio->audio(), true);
	if (lnkDocument) DocumentSaveLink::doSave(lnkDocument->document(), true);
}

void OverviewInner::openContextFile() {
	VideoLink *lnkVideo = dynamic_cast<VideoLink*>(_contextMenuLnk.data());
	AudioLink *lnkAudio = dynamic_cast<AudioLink*>(_contextMenuLnk.data());
	DocumentLink *lnkDocument = dynamic_cast<DocumentLink*>(_contextMenuLnk.data());
	if (lnkVideo) VideoOpenLink(lnkVideo->video()).onClick(Qt::LeftButton);
	if (lnkAudio) AudioOpenLink(lnkAudio->audio()).onClick(Qt::LeftButton);
	if (lnkDocument) DocumentOpenLink(lnkDocument->document()).onClick(Qt::LeftButton);
}

void OverviewInner::onMenuDestroy(QObject *obj) {
	if (_menu == obj) {
		_menu = 0;
		updateMsg(App::contextItem());
		if (_selectedMsgId > 0) updateMsg(App::histItemById(_selectedMsgId));
	}
}

void OverviewInner::getSelectionState(int32 &selectedForForward, int32 &selectedForDelete) const {
	selectedForForward = selectedForDelete = 0;
	for (SelectedItems::const_iterator i = _selected.cbegin(), e = _selected.cend(); i != e; ++i) {
		if (i.value() == FullItemSel) {
			++selectedForDelete;
			if (i.key() > 0) {
				++selectedForForward;
			}
		}
	}
	if (!selectedForDelete && !selectedForForward && !_selected.isEmpty()) { // text selection
		selectedForForward = -1;
	}
}

void OverviewInner::clearSelectedItems(bool onlyTextSelection) {
	if (!_selected.isEmpty() && (!onlyTextSelection || _selected.cbegin().value() != FullItemSel)) {
		_selected.clear();
		_overview->updateTopBarSelection();
		_overview->update();
	}
}

void OverviewInner::fillSelectedItems(SelectedItemSet &sel, bool forDelete) {
	if (_selected.isEmpty() || _selected.cbegin().value() != FullItemSel) return;

	for (SelectedItems::const_iterator i = _selected.cbegin(), e = _selected.cend(); i != e; ++i) {
		HistoryItem *item = App::histItemById(i.key());
		if (dynamic_cast<HistoryMessage*>(item) && item->id > 0) {
			sel.insert(item->id, item);
		}
	}
}

void OverviewInner::onTouchSelect() {
	_touchSelect = true;
	dragActionStart(_touchPos);
}

void OverviewInner::onTouchScrollTimer() {
	uint64 nowTime = getms();
	if (_touchScrollState == TouchScrollAcceleration && _touchWaitingAcceleration && (nowTime - _touchAccelerationTime) > 40) {
		_touchScrollState = TouchScrollManual;
		touchResetSpeed();
	} else if (_touchScrollState == TouchScrollAuto || _touchScrollState == TouchScrollAcceleration) {
		int32 elapsed = int32(nowTime - _touchTime);
		QPoint delta = _touchSpeed * elapsed / 1000;
		bool hasScrolled = _overview->touchScroll(delta);

		if (_touchSpeed.isNull() || !hasScrolled) {
			_touchScrollState = TouchScrollManual;
			_touchScroll = false;
			_touchScrollTimer.stop();
		} else {
			_touchTime = nowTime;
		}
		touchDeaccelerate(elapsed);
	}
}

void OverviewInner::mediaOverviewUpdated(bool fromResize) {
	int32 oldHeight = _height;
	if (_type != OverviewPhotos && _type != OverviewAudioDocuments) {
		History::MediaOverview &o(_hist->_overview[_type]);
		int32 l = o.size();
		_items.reserve(2 * l); // day items

		int32 y = 0, in = 0;
		int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
		bool allGood = true;
		QDate prevDate;
		for (int32 i = 0; i < l; ++i) {
			MsgId msgid = o.at(l - i - 1);
			if (allGood) {
				if (_items.size() > in && _items.at(in).msgid == msgid) {
					prevDate = _items.at(in).date;
					if (fromResize) {
						HistoryItem *item = App::histItemById(msgid);
						HistoryMedia *media = item ? item->getMedia(true) : 0;
						if (media) {
							y += media->countHeight(item, w) + st::msgMargin.top() + st::msgMargin.bottom(); // item height
						}
						_items[in].y = y;
					} else {
						y = _items.at(in).y;
					}
					++in;
					continue;
				}
				if (_items.size() > in + 1 && !_items.at(in).msgid && _items.at(in + 1).msgid == msgid) { // day item
					if (fromResize) {
						y += st::msgServiceFont->height + st::msgServicePadding.top() + st::msgServicePadding.bottom() + st::msgServiceMargin.top() + st::msgServiceMargin.bottom(); // day item height
						_items[in].y = y;
					}
					++in;
					prevDate = _items.at(in).date;
					if (fromResize) {
						HistoryItem *item = App::histItemById(msgid);
						HistoryMedia *media = item ? item->getMedia(true) : 0;
						if (media) {
							y += media->countHeight(item, w) + st::msgMargin.top() + st::msgMargin.bottom(); // item height
						}
						_items[in].y = y;
					} else {
						y = _items.at(in).y;
					}
					++in;
					continue;
				}
				allGood = false;
			}
			HistoryItem *item = App::histItemById(msgid);
			HistoryMedia *media = item ? item->getMedia(true) : 0;
			if (!media) continue;

			QDate date = item->date.date();
			if (in > 0) {
				if (date != prevDate) { // add day item
					y += st::msgServiceFont->height + st::msgServicePadding.top() + st::msgServicePadding.bottom() + st::msgServiceMargin.top() + st::msgServiceMargin.bottom(); // day item height
					if (_items.size() > in) {
						_items[in].msgid = 0;
						_items[in].date = prevDate;
						_items[in].y = y;
					} else {
						_items.push_back(CachedItem(0, prevDate, y));
					}
					++in;
					prevDate = date;
				}
			} else {
				prevDate = date;
			}
			media->initDimensions(item);
			y += media->countHeight(item, w) + st::msgMargin.top() + st::msgMargin.bottom(); // item height
			if (_items.size() > in) {
				_items[in].msgid = msgid;
				_items[in].date = date;
				_items[in].y = y;
			} else {
				_items.push_back(CachedItem(msgid, date, y));
			}
			++in;
		}
		if (!_items.isEmpty()) {
			y += st::msgServiceFont->height + st::msgServicePadding.top() + st::msgServicePadding.bottom() + st::msgServiceMargin.top() + st::msgServiceMargin.bottom(); // day item height
			if (_items.size() > in) {
				_items[in].msgid = 0;
				_items[in].date = prevDate;
				_items[in].y = y;
			} else {
				_items.push_back(CachedItem(0, prevDate, y));
			}
			_items.resize(++in);
		}
		if (_height != y) {
			_height = y;
			if (!fromResize) {
				_addToY = (_type == OverviewAudioDocuments) ? st::playlistPadding : ((_height < _minHeight) ? (_minHeight - _height) : 0);
				resize(width(), _minHeight > _height ? _minHeight : _height);
			}
		}
	}

	fixItemIndex(_dragSelFromIndex, _dragSelFrom);
	fixItemIndex(_dragSelToIndex, _dragSelTo);
	fixItemIndex(_mousedItemIndex, _mousedItem);
	fixItemIndex(_dragItemIndex, _dragItem);

	if (!fromResize) {
		resizeEvent(0);
		if (_height != oldHeight) {
			_overview->scrollBy(_height - oldHeight);
		}
	}
}

void OverviewInner::changingMsgId(HistoryItem *row, MsgId newId) {
	if (_dragSelFrom == row->id) _dragSelFrom = newId;
	if (_dragSelTo == row->id) _dragSelTo = newId;
	if (_mousedItem == row->id) _mousedItem = newId;
	if (_dragItem == row->id) _dragItem = newId;
	for (SelectedItems::iterator i = _selected.begin(), e = _selected.end(); i != e; ++i) {
		if (i.key() == row->id) {
			uint32 sel = i.value();
			_selected.erase(i);
			_selected.insert(newId, sel);
			break;
		}
	}
	for (CachedItems::iterator i = _items.begin(), e = _items.end(); i != e; ++i) {
		if (i->msgid == row->id) {
			i->msgid = newId;
			break;
		}
	}
}

void OverviewInner::itemRemoved(HistoryItem *item) {
	if (_dragItem == item->id) {
		dragActionCancel();
	}

	SelectedItems::iterator i = _selected.find(item->id);
	if (i != _selected.cend()) {
		_selected.erase(i);
		_overview->updateTopBarSelection();
	}

	onUpdateSelected();

	if (_dragSelFrom == item->id) {
		_dragSelFrom = 0;
		_dragSelFromIndex = -1;
	}
	if (_dragSelTo == item->id) {
		_dragSelTo = 0;
		_dragSelToIndex = -1;
	}
	updateDragSelection(_dragSelFrom, _dragSelFromIndex, _dragSelTo, _dragSelToIndex, _dragSelecting);

	parentWidget()->update();
}

void OverviewInner::itemResized(HistoryItem *item, bool scrollToIt) {
	if (_type != OverviewPhotos && _type != OverviewAudioDocuments) {
		HistoryMedia *media = item ? item->getMedia(true) : 0;
		if (!media) return;

		for (int32 i = 0, l = _items.size(); i < l; ++i) {
			if (_items[i].msgid == item->id) {
				int32 from = 0;
				if (i > 0) from = _items[i - 1].y;

				int32 oldh = _items[i].y - from;
				int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
				int32 newh = media->countHeight(item, w) + st::msgMargin.top() + st::msgMargin.bottom(); // item height
				if (oldh != newh) {
					newh -= oldh;
					for (int32 j = i; j < l; ++j) {
						_items[j].y += newh;
					}
					_height = _items[l - 1].y;
					_addToY = (_type == OverviewAudioDocuments) ? st::playlistPadding : ((_height < _minHeight) ? (_minHeight - _height) : 0);
					resize(width(), _minHeight > _height ? _minHeight : _height);
					if (scrollToIt) {
						if (_addToY + _height - from > _scroll->scrollTop() + _scroll->height()) {
							_scroll->scrollToY(_addToY + _height - from - _scroll->height());
						}
						if (_addToY + _height - _items[i].y < _scroll->scrollTop()) {
							_scroll->scrollToY(_addToY + _height - _items[i].y);
						}
					}
					parentWidget()->update();
				}
				break;
			}
		}
	}
}

void OverviewInner::msgUpdated(const HistoryItem *msg) {
	if (!msg || _hist != msg->history()) return;
	MsgId msgid = msg->id;
	if (_hist->_overviewIds[_type].constFind(msgid) != _hist->_overviewIds[_type].cend()) {
		if (_type == OverviewPhotos) {
			int32 index = _hist->_overview[_type].indexOf(msgid);
			if (index >= 0) {
				float64 w = (float64(width() - st::overviewPhotoSkip) / _photosInRow);
				int32 vsize = (_vsize + st::overviewPhotoSkip);
				int32 row = (_photosToAdd + index) / _photosInRow, col = (_photosToAdd + index) % _photosInRow;
				update(int32(col * w), _addToY + int32(row * vsize), qCeil(w), vsize);
			}
		} else if (_type == OverviewAudioDocuments) {
			int32 index = _hist->_overview[_type].indexOf(msgid);
			if (index >= 0) {
				update(_audioLeft, _addToY + int32(index * _audioHeight), _audioWidth, _audioHeight);
			}
		} else {
			for (int32 i = 0, l = _items.size(); i != l; ++i) {
				if (_items[i].msgid == msgid) {
					HistoryMedia *media = msg->getMedia(true);
					int32 w = _width - st::msgMargin.left() - st::msgMargin.right();
					if (media) update(0, _addToY + _height - _items[i].y, _width, media->countHeight(msg, w) + st::msgMargin.top() + st::msgMargin.bottom());
					break;
				}
			}
		}
	}
}

void OverviewInner::showAll(bool recountHeights) {
	int32 newHeight = height();
	if (_type == OverviewPhotos) {
		_photosInRow = int32(width() - st::overviewPhotoSkip) / int32(st::overviewPhotoMinSize + st::overviewPhotoSkip);
		_vsize = (int32(width() - st::overviewPhotoSkip) / _photosInRow) - st::overviewPhotoSkip;
		int32 count = _hist->_overview[_type].size(), fullCount = _hist->_overviewCount[_type];
		if (fullCount > 0) {
			int32 cnt = count - (fullCount % _photosInRow);
			if (cnt < 0) cnt += _photosInRow;
			_photosToAdd = (_photosInRow - (cnt % _photosInRow)) % _photosInRow;
		} else {
			_photosToAdd = 0;
		}
		int32 rows = ((_photosToAdd + count) / _photosInRow) + (((_photosToAdd + count) % _photosInRow) ? 1 : 0);
		newHeight = _height = (_vsize + st::overviewPhotoSkip) * rows + st::overviewPhotoSkip;
	} else if (_type == OverviewAudioDocuments) {
		int32 count = _hist->_overview[_type].size(), fullCount = _hist->_overviewCount[_type];
		newHeight = _height = count * _audioHeight + 2 * st::playlistPadding;
	} else {
		if (recountHeights && _type == OverviewVideos) { // recount heights because of captions
			mediaOverviewUpdated(true);
		}
		newHeight = _height;
	}
	_addToY = (_type == OverviewAudioDocuments) ? st::playlistPadding : ((_height < _minHeight) ? (_minHeight - _height) : 0);
	if (newHeight < _minHeight) {
		newHeight = _minHeight;
	}
	if (height() != newHeight) {
		resize(width(), newHeight);
	}
}

OverviewInner::~OverviewInner() {
	_dragAction = NoDrag;
}

OverviewWidget::OverviewWidget(QWidget *parent, const PeerData *peer, MediaOverviewType type) : QWidget(parent)
, _scroll(this, st::historyScroll, false)
, _inner(this, &_scroll, peer, type)
, _noDropResizeIndex(false)
, _showing(false)
, _scrollSetAfterShow(0)
, _scrollDelta(0)
, _selCount(0) {
	_scroll.setFocusPolicy(Qt::NoFocus);
	_scroll.setWidget(&_inner);
	_scroll.move(0, 0);
	_inner.move(0, 0);

	updateScrollColors();

	_scroll.show();
	connect(&_scroll, SIGNAL(scrolled()), &_inner, SLOT(onUpdateSelected()));
	connect(&_scroll, SIGNAL(scrolled()), this, SLOT(onScroll()));

	connect(&_scrollTimer, SIGNAL(timeout()), this, SLOT(onScrollTimer()));
	_scrollTimer.setSingleShot(false);

	connect(App::main()->player(), SIGNAL(playerSongChanged(MsgId)), this, SLOT(onPlayerSongChanged(MsgId)));

	switchType(type);
}

void OverviewWidget::clear() {
	_inner.clear();
}

void OverviewWidget::onScroll() {
	MTP::clearLoaderPriorities();
	if (_scroll.scrollTop() < _scroll.height() * 5) {
		if (App::main()) {
			App::main()->loadMediaBack(peer(), type(), true);
		}
	}
	if (!_noDropResizeIndex) {
		_inner.dropResizeIndex();
	}
}

void OverviewWidget::resizeEvent(QResizeEvent *e) {
	int32 st = _scroll.scrollTop();
	_scroll.resize(size());
	int32 newScrollTop = _inner.resizeToWidth(width(), st, height());
	if (int32 addToY = App::main() ? App::main()->contentScrollAddToY() : 0) {
		newScrollTop += addToY;
	}
	if (newScrollTop != _scroll.scrollTop()) {
		_noDropResizeIndex = true;
		_scroll.scrollToY(newScrollTop);
		_noDropResizeIndex = false;
	}
}

void OverviewWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	if (animating() && _showing) {
		p.setOpacity(a_bgAlpha.current());
		p.drawPixmap(a_bgCoord.current(), 0, _bgAnimCache);
		p.setOpacity(a_alpha.current());
		p.drawPixmap(a_coord.current(), 0, _animCache);
		return;
	}

	QRect r(e->rect());
	if (type() == OverviewPhotos || type() == OverviewAudioDocuments) {
		p.fillRect(r, st::white->b);
	} else {
		bool hasTopBar = !App::main()->topBar()->isHidden(), hasPlayer = !App::main()->player()->isHidden();
		QRect fill(0, 0, width(), App::main()->height());
		int fromy = (hasTopBar ? (-st::topBarHeight) : 0) + (hasPlayer ? (-st::playerHeight) : 0), x = 0, y = 0;
		QPixmap cached = App::main()->cachedBackground(fill, x, y);
		if (cached.isNull()) {
			const QPixmap &pix(*cChatBackground());
			if (cTileBackground()) {
				int left = r.left(), top = r.top(), right = r.left() + r.width(), bottom = r.top() + r.height();
				float64 w = pix.width() / cRetinaFactor(), h = pix.height() / cRetinaFactor();
				int sx = qFloor(left / w), sy = qFloor((top - fromy) / h), cx = qCeil(right / w), cy = qCeil((bottom - fromy) / h);
				for (int i = sx; i < cx; ++i) {
					for (int j = sy; j < cy; ++j) {
						p.drawPixmap(QPointF(i * w, fromy + j * h), pix);
					}
				}
			} else {
				bool smooth = p.renderHints().testFlag(QPainter::SmoothPixmapTransform);
				p.setRenderHint(QPainter::SmoothPixmapTransform);

				QRect to, from;
				App::main()->backgroundParams(fill, to, from);
				to.moveTop(to.top() + fromy);
				p.drawPixmap(to, pix, from);

				if (!smooth) p.setRenderHint(QPainter::SmoothPixmapTransform, false);
			}
		} else {
			p.drawPixmap(x, fromy + y, cached);
		}
	}
}

void OverviewWidget::contextMenuEvent(QContextMenuEvent *e) {
	return _inner.showContextMenu(e);
}

void OverviewWidget::scrollBy(int32 add) {
	if (_scroll.isHidden()) {
		_scrollSetAfterShow += add;
	} else {
		_scroll.scrollToY(_scroll.scrollTop() + add);
	}
}

void OverviewWidget::paintTopBar(QPainter &p, float64 over, int32 decreaseWidth) {
	if (animating() && _showing) {
		p.setOpacity(a_bgAlpha.current());
		p.drawPixmap(a_bgCoord.current(), 0, _bgAnimTopBarCache);
		p.setOpacity(a_alpha.current());
		p.drawPixmap(a_coord.current(), 0, _animTopBarCache);
	} else {
		p.setOpacity(st::topBarBackAlpha + (1 - st::topBarBackAlpha) * over);
		p.drawPixmap(QPoint(st::topBarBackPadding.left(), (st::topBarHeight - st::topBarBackImg.pxHeight()) / 2), App::sprite(), st::topBarBackImg);
		p.setFont(st::topBarBackFont->f);
		p.setPen(st::topBarBackColor->p);
		p.drawText(st::topBarBackPadding.left() + st::topBarBackImg.pxWidth() + st::topBarBackPadding.right(), (st::topBarHeight - st::topBarBackFont->height) / 2 + st::topBarBackFont->ascent, _header);
	}
}

void OverviewWidget::topBarShadowParams(int32 &x, float64 &o) {
	if (animating() && a_coord.current() >= 0) {
		x = a_coord.current();
		o = a_alpha.current();
	}
}

void OverviewWidget::topBarClick() {
	App::main()->showBackFromStack();
}

PeerData *OverviewWidget::peer() const {
	return _inner.peer();
}

MediaOverviewType OverviewWidget::type() const {
	return _inner.type();
}

void OverviewWidget::switchType(MediaOverviewType type) {
	_selCount = 0;
	_inner.setSelectMode(false);
	_inner.switchType(type);
	switch (type) {
	case OverviewPhotos: _header = lang(lng_profile_photos_header); break;
	case OverviewVideos: _header = lang(lng_profile_videos_header); break;
	case OverviewDocuments: _header = lang(lng_profile_files_header); break;
	case OverviewAudios: _header = lang(lng_profile_audios_header); break;
	case OverviewAudioDocuments: _header = lang(lng_profile_audio_files_header); break;
	}
	noSelectingScroll();
	App::main()->topBar()->showSelected(0);
	updateTopBarSelection();
	_scroll.scrollToY(_scroll.scrollTopMax());
	onScroll();
}

void OverviewWidget::updateTopBarSelection() {
	int32 selectedForForward, selectedForDelete;
	_inner.getSelectionState(selectedForForward, selectedForDelete);
	_selCount = selectedForDelete ? selectedForDelete : selectedForForward;
	_inner.setSelectMode(_selCount > 0);
	if (App::main()) {
		App::main()->topBar()->showSelected(_selCount > 0 ? _selCount : 0);
		App::main()->topBar()->update();
	}
	if (App::wnd() && !App::wnd()->layerShown()) {
		_inner.setFocus();
	}
	update();
}

int32 OverviewWidget::lastWidth() const {
	return width();
}

int32 OverviewWidget::lastScrollTop() const {
	return _scroll.scrollTop();
}

int32 OverviewWidget::countBestScroll() const {
	if (type() == OverviewAudioDocuments && audioPlayer()) {
		SongMsgId playing;
		AudioPlayerState playingState = AudioPlayerStopped;
		audioPlayer()->currentState(&playing, &playingState);
		if (playing) {
			int32 top = _inner.itemTop(playing.msgId);
			if (top >= 0) {
				return snap(top - int(_scroll.height() - (st::mediaPadding.top() + st::mediaThumbSize + st::mediaPadding.bottom())) / 2, 0, _scroll.scrollTopMax());
			}
		}
	}
	return _scroll.scrollTopMax();
}

void OverviewWidget::fastShow(bool back, int32 lastScrollTop) {
	stopGif();
	resizeEvent(0);
	_scrollSetAfterShow = (lastScrollTop < 0 ? countBestScroll() : lastScrollTop);
	show();
	_inner.setFocus();
	doneShow();
}

void OverviewWidget::animShow(const QPixmap &bgAnimCache, const QPixmap &bgAnimTopBarCache, bool back, int32 lastScrollTop) {
	stopGif();
	_bgAnimCache = bgAnimCache;
	_bgAnimTopBarCache = bgAnimTopBarCache;
	resizeEvent(0);
	_scroll.scrollToY(lastScrollTop < 0 ? countBestScroll() : lastScrollTop);
	_animCache = myGrab(this, rect());
	App::main()->topBar()->stopAnim();
	_animTopBarCache = myGrab(App::main()->topBar(), QRect(0, 0, width(), st::topBarHeight));
	App::main()->topBar()->startAnim();
	_scrollSetAfterShow = _scroll.scrollTop();
	_scroll.hide();
	a_coord = back ? anim::ivalue(-st::introSlideShift, 0) : anim::ivalue(st::introSlideShift, 0);
	a_alpha = anim::fvalue(0, 1);
	a_bgCoord = back ? anim::ivalue(0, st::introSlideShift) : anim::ivalue(0, -st::introSlideShift);
	a_bgAlpha = anim::fvalue(1, 0);
	anim::start(this);
	_showing = true;
	show();
	_inner.setFocus();
	App::main()->topBar()->update();
}

bool OverviewWidget::animStep(float64 ms) {
	float64 fullDuration = st::introSlideDelta + st::introSlideDuration, dt = ms / fullDuration;
	float64 dt1 = (ms > st::introSlideDuration) ? 1 : (ms / st::introSlideDuration), dt2 = (ms > st::introSlideDelta) ? (ms - st::introSlideDelta) / (st::introSlideDuration) : 0;
	bool res = true;
	if (dt2 >= 1) {
		res = _showing = false;
		a_bgCoord.finish();
		a_bgAlpha.finish();
		a_coord.finish();
		a_alpha.finish();
		_bgAnimCache = _animCache = _animTopBarCache = _bgAnimTopBarCache = QPixmap();
		App::main()->topBar()->stopAnim();
		doneShow();
	} else {
		a_bgCoord.update(dt1, st::introHideFunc);
		a_bgAlpha.update(dt1, st::introAlphaHideFunc);
		a_coord.update(dt2, st::introShowFunc);
		a_alpha.update(dt2, st::introAlphaShowFunc);
	}
	update();
	App::main()->topBar()->update();
	return res;
}

void OverviewWidget::doneShow() {
	_scroll.show();
	_scroll.scrollToY(_scrollSetAfterShow);
	activate();
	onScroll();
}

void OverviewWidget::mediaOverviewUpdated(PeerData *p, MediaOverviewType t) {
	if (peer() == p && t == type()) {
		_inner.mediaOverviewUpdated();
		onScroll();
		updateTopBarSelection();
	}
}

void OverviewWidget::changingMsgId(HistoryItem *row, MsgId newId) {
	if (peer() == row->history()->peer) {
		_inner.changingMsgId(row, newId);
	}
}

void OverviewWidget::msgUpdated(PeerId p, const HistoryItem *msg) {
	if (peer()->id == p) {
		_inner.msgUpdated(msg);
	}
}

void OverviewWidget::itemRemoved(HistoryItem *row) {
	if (row->history()->peer == peer()) {
		_inner.itemRemoved(row);
	}
}

void OverviewWidget::itemResized(HistoryItem *row, bool scrollToIt) {
	if (!row || row->history()->peer == peer()) {
		_inner.itemResized(row, scrollToIt);
	}
}

void OverviewWidget::fillSelectedItems(SelectedItemSet &sel, bool forDelete) {
	_inner.fillSelectedItems(sel, forDelete);
}

void OverviewWidget::updateScrollColors() {
	if (!App::historyScrollBarColor()) return;
	_scroll.updateColors(App::historyScrollBarColor(), App::historyScrollBgColor(), App::historyScrollBarOverColor(), App::historyScrollBgOverColor());
}

OverviewWidget::~OverviewWidget() {
	onClearSelected();
	updateTopBarSelection();
}

void OverviewWidget::activate() {
	_inner.setFocus();
}

QPoint OverviewWidget::clampMousePosition(QPoint point) {
	if (point.x() < 0) {
		point.setX(0);
	} else if (point.x() >= _scroll.width()) {
		point.setX(_scroll.width() - 1);
	}
	if (point.y() < _scroll.scrollTop()) {
		point.setY(_scroll.scrollTop());
	} else if (point.y() >= _scroll.scrollTop() + _scroll.height()) {
		point.setY(_scroll.scrollTop() + _scroll.height() - 1);
	}
	return point;
}

void OverviewWidget::onScrollTimer() {
	int32 d = (_scrollDelta > 0) ? qMin(_scrollDelta * 3 / 20 + 1, int32(MaxScrollSpeed)) : qMax(_scrollDelta * 3 / 20 - 1, -int32(MaxScrollSpeed));
	_scroll.scrollToY(_scroll.scrollTop() + d);
}

void OverviewWidget::onPlayerSongChanged(MsgId msgId) {
	if (type() == OverviewAudioDocuments) {
//		int32 top = _inner.itemTop(msgId);
//		if (top > 0) {
//			_scroll.scrollToY(snap(top - int(_scroll.height() - (st::mediaPadding.top() + st::mediaThumbSize + st::mediaPadding.bottom())) / 2, 0, _scroll.scrollTopMax()));
//		}
	}
}

void OverviewWidget::checkSelectingScroll(QPoint point) {
	if (point.y() < _scroll.scrollTop()) {
		_scrollDelta = point.y() - _scroll.scrollTop();
	} else if (point.y() >= _scroll.scrollTop() + _scroll.height()) {
		_scrollDelta = point.y() - _scroll.scrollTop() - _scroll.height() + 1;
	} else {
		_scrollDelta = 0;
	}
	if (_scrollDelta) {
		_scrollTimer.start(15);
	} else {
		_scrollTimer.stop();
	}
}

void OverviewWidget::noSelectingScroll() {
	_scrollTimer.stop();
}

bool OverviewWidget::touchScroll(const QPoint &delta) {
	int32 scTop = _scroll.scrollTop(), scMax = _scroll.scrollTopMax(), scNew = snap(scTop - delta.y(), 0, scMax);
	if (scNew == scTop) return false;

	_scroll.scrollToY(scNew);
	return true;
}

void OverviewWidget::onForwardSelected() {
	App::main()->forwardLayer(true);
}

void OverviewWidget::onDeleteSelected() {
	SelectedItemSet sel;
	_inner.fillSelectedItems(sel);
	if (sel.isEmpty()) return;

	App::main()->deleteLayer(sel.size());
}

void OverviewWidget::onDeleteSelectedSure() {
	SelectedItemSet sel;
	_inner.fillSelectedItems(sel);
	if (sel.isEmpty()) return;

	QVector<MTPint> ids;
	for (SelectedItemSet::const_iterator i = sel.cbegin(), e = sel.cend(); i != e; ++i) {
		if (i.value()->id > 0) {
			ids.push_back(MTP_int(i.value()->id));
		}
	}

	if (!ids.isEmpty()) {
		App::main()->deleteMessages(ids);
	}

	onClearSelected();
	for (SelectedItemSet::const_iterator i = sel.cbegin(), e = sel.cend(); i != e; ++i) {
		i.value()->destroy();
	}
	if (App::main() && App::main()->peer() == peer()) {
		App::main()->itemResized(0);
	}
	App::wnd()->hideLayer();
}

void OverviewWidget::onDeleteContextSure() {
	HistoryItem *item = App::contextItem();
	if (!item || item->itemType() != HistoryItem::MsgType) {
		return;
	}

	if (item->id > 0) {
		App::main()->deleteMessages(QVector<MTPint>(1, MTP_int(item->id)));
	}
	item->destroy();
	if (App::main() && App::main()->peer() == peer()) {
		App::main()->itemResized(0);
	}
	App::wnd()->hideLayer();
}

void OverviewWidget::onClearSelected() {
	_inner.clearSelectedItems();
}
