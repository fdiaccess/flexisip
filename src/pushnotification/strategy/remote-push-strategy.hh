/*
    Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010-2022 Belledonne Communications SARL, All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <chrono>
#include <memory>

#include "flexisip/fork-context/branch-info.hh"
#include "flexisip/sofia-wrapper/timer.hh"

#include "strategy.hh"

namespace flexisip {
namespace pushnotification {

class RemotePushStrategy : public Strategy,
                           public BranchInfoListener,
                           public std::enable_shared_from_this<RemotePushStrategy> {
public:
	template <typename... Args>
	static std::shared_ptr<RemotePushStrategy> make(Args&&... args) {
		return std::shared_ptr<RemotePushStrategy>{new RemotePushStrategy{std::forward<Args>(args)...}};
	};

	// Set the interval between two subsequent notifications when this strategy is used for call invite notification.
	void setCallPushInterval(std::chrono::seconds interval) noexcept {
		mCallPushInterval = interval;
	}

	bool pushRepetitionEnabled() const noexcept {
		return mCallPushInterval.count() > 0;
	}

	void sendMessageNotification(const std::shared_ptr<const PushInfo>& pInfo) override;
	void sendCallNotification(const std::shared_ptr<const PushInfo>& pInfo) override;

private:
	// Private ctor
	RemotePushStrategy(const std::shared_ptr<sofiasip::SuRoot>& root,
	                   const std::shared_ptr<Service>& service,
	                   const std::shared_ptr<BranchInfo>& br)
	    : Strategy{root, service}, mBranchInfo{br} {
	}

	// Private methods
	void onBranchCanceled(const std::shared_ptr<BranchInfo>& br, ForkStatus cancelReason) noexcept override;
	void onBranchCompleted(const std::shared_ptr<BranchInfo>& br) noexcept override;

	// Attributes
	std::weak_ptr<BranchInfo> mBranchInfo{};
	std::chrono::seconds mCallPushInterval{2};
	std::chrono::seconds mCallRingingTimeout{45};
	std::shared_ptr<PushInfo> mCallPushInfo{};
	std::unique_ptr<sofiasip::Timer> mCallRingingTimeoutTimer{};
};

inline std::ostream& operator<<(std::ostream& os, const RemotePushStrategy* s) noexcept {
	return (os << "RemotePushStrategy[" << static_cast<const void*>(s) << "]");
}
inline std::ostream& operator<<(std::ostream& os, const std::shared_ptr<RemotePushStrategy>& s) noexcept {
	return operator<<(os, s.get());
}

} // namespace pushnotification
} // namespace flexisip
