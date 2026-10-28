/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_schedule_box.h"

#include "api/api_common.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/components/scheduled_messages.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_messages.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "base/event_filter.h"
#include "base/qt/qt_key_modifiers.h"
#include "base/unixtime.h"
#include "lottie/lottie_icon.h"
#include "ui/controls/warning_tooltip.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "main/main_session.h"
#include "menu/menu_send.h"
#include "settings/sections/settings_premium.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace HistoryView::details {

not_null<Main::Session*> SessionFromShow(
		const std::shared_ptr<ChatHelpers::Show> &show) {
	return &show->session();
}

} // namespace HistoryView::details

namespace HistoryView {
namespace {

[[nodiscard]] std::shared_ptr<Ui::DynamicImage> ItemThumbnail(
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return nullptr;
	} else if (const auto photo = media->photo()) {
		return Ui::MakePhotoThumbnail(photo, item->fullId());
	} else if (const auto document = media->document()) {
		if (document->isVideoFile()) {
			return Ui::MakeDocumentThumbnail(document, item->fullId());
		}
	}
	return nullptr;
}

[[nodiscard]] Fn<void(QDate, Ui::CalendarImageSetter)> ScheduledImageForDate(
		not_null<History*> history,
		MsgId topicRootId) {
	struct State {
		base::flat_map<QDate, std::shared_ptr<Ui::DynamicImage>> images;
		Ui::CalendarImageSetter setter;
		bool subscribed = false;
		rpl::lifetime lifetime;
	};
	const auto state = std::make_shared<State>();
	const auto raw = state.get();
	const auto session = &history->session();

	const auto refresh = [=] {
		const auto topic = topicRootId
			? history->peer->forumTopicFor(topicRootId)
			: nullptr;
		auto previous = base::take(raw->images);
		if (!topicRootId || topic) {
			const auto &scheduled = session->scheduledMessages();
			const auto list = topic
				? scheduled.list(topic)
				: scheduled.list(history);
			for (const auto &fullId : list.ids) {
				const auto item = session->data().message(fullId);
				if (!item
					|| item->date() == Api::kScheduledUntilOnlineTimestamp) {
					continue;
				}
				const auto date = base::unixtime::parse(item->date()).date();
				if (raw->images.contains(date)) {
					continue;
				}
				if (auto image = ItemThumbnail(item)) {
					raw->images.emplace(date, std::move(image));
				}
			}
		}
		if (const auto setter = raw->setter) {
			for (const auto &[date, image] : raw->images) {
				setter(date, image);
			}
			for (const auto &[date, image] : previous) {
				if (!raw->images.contains(date)) {
					setter(date, nullptr);
				}
			}
		}
	};

	return [=](QDate date, Ui::CalendarImageSetter setter) {
		state->setter = std::move(setter);
		if (!state->subscribed) {
			state->subscribed = true;
			rpl::single(rpl::empty) | rpl::then(
				session->scheduledMessages().updates(history)
			) | rpl::on_next(refresh, state->lifetime);
		} else {
			for (const auto &[known, image] : state->images) {
				state->setter(known, image);
			}
		}
	};
}

} // namespace

ScheduleBoxStyleArgs::ScheduleBoxStyleArgs()
: topButtonStyle(&st::infoTopBarMenu)
, popupMenuStyle(&st::popupMenuWithIcons)
, chooseDateTimeArgs({}) {
}

TimeId DefaultScheduleTime() {
	return base::unixtime::now() + 600;
}

bool CanScheduleUntilOnline(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isSelf()
			&& !user->isBot()
			&& !user->lastseen().isHidden()
			&& !user->starsPerMessageChecked()
			&& !user->isNotificationsUser();
	}
	return false;
}

void ScheduleBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		std::shared_ptr<ChatHelpers::Show> maybeShow,
		const Api::SendOptions &initialOptions,
		const SendMenu::Details &details,
		Fn<void(Api::SendOptions)> done,
		TimeId time,
		ScheduleBoxStyleArgs style) {
	const auto repeat = std::make_shared<TimeId>(
		initialOptions.scheduleRepeatPeriod);
	const auto submit = [=](Api::SendOptions options) {
		if (!options.scheduled) {
			return;
		}
		if (repeat) {
			options.scheduleRepeatPeriod = *repeat;
		}
		const auto copy = done;
		box->closeBox();
		copy(options);
	};
	const auto with = [=](TimeId scheduled) {
		auto result = initialOptions;
		result.scheduled = scheduled;
		return result;
	};
	const auto history = details.barePeerId
		? session->data().historyLoaded(PeerId(details.barePeerId))
		: nullptr;
	auto descriptor = Ui::ChooseDateTimeBox(box, {
		.title = (details.type == SendMenu::Type::Reminder
			? tr::lng_remind_title()
			: tr::lng_schedule_title()),
		.submit = tr::lng_schedule_button(),
		.done = [=](TimeId result) { submit(with(result)); },
		.time = time,
		.style = style.chooseDateTimeArgs,
		.dynamicImageForDate = (history
			? ScheduledImageForDate(history, MsgId(details.bareTopicRootId))
			: nullptr),
	});

	if (repeat) {
		const auto boxShow = box->uiShow();
		const auto showPremiumPromo = [=] {
			if (session->premium()) {
				return false;
			}
			Settings::ShowPremiumPromoToast(
				Main::MakeSessionShow(boxShow, session),
				ChatHelpers::ResolveWindowDefault(),
				tr::lng_schedule_repeat_promo(
					tr::now,
					lt_link,
					tr::link(
						tr::bold(
							tr::lng_schedule_repeat_promo_link(tr::now))),
					tr::rich),
				u"schedule_repeat"_q);
			return true;
		};
		auto locked = Data::AmPremiumValue(
			session
		) | rpl::map([=](bool premium) {
			return !premium;
		});
		const auto row = box->addRow(Ui::ChooseRepeatPeriod(box, {
			.value = session->premium() ? *repeat : TimeId(),
			.locked = std::move(locked),
			.filter = showPremiumPromo,
			.changed = [=](TimeId value) { *repeat = value; },
			.test = session->isTestMode(),
		}), st::scheduleRepeatMargin, style::al_top);
		std::move(descriptor.width) | rpl::on_next([=](int width) {
			row->setNaturalWidth(width);
		}, row->lifetime());
	}

	using namespace SendMenu;
	const auto childType = (details.type == Type::Disabled)
		? Type::Disabled
		: Type::SilentOnly;
	const auto childDetails = Details{
		.type = childType,
		.effectAllowed = details.effectAllowed,
	};
	const auto sendAction = crl::guard(box, [=](Action action, Details) {
		Expects(action.type == ActionType::Send);

		auto options = with(descriptor.collect());
		if (action.options.silent) {
			options.silent = action.options.silent;
		}
		if (action.options.effectId) {
			options.effectId = action.options.effectId;
		}
		submit(options);
	});
	SetupMenuAndShortcuts(
		descriptor.submit.data(),
		maybeShow,
		[=] { return childDetails; },
		sendAction);
}

} // namespace HistoryView
