// models.hpp — umbrella header: pulls in every pulse::models::<name> model.
//
// Include this when a translation unit needs more than one model (the index
// aggregator, bootstrap, etc.) instead of listing each header individually.
// Each included header declares its own namespace, collection name,
// ensureIndexes(), and the ported statics/instance helpers.
#pragma once

#include "pulse/models/user.hpp"
#include "pulse/models/otp.hpp"
#include "pulse/models/session.hpp"
#include "pulse/models/post.hpp"
#include "pulse/models/comment.hpp"
#include "pulse/models/like.hpp"
#include "pulse/models/follow.hpp"
#include "pulse/models/notification.hpp"
#include "pulse/models/conversation.hpp"
#include "pulse/models/message.hpp"
#include "pulse/models/reel.hpp"
#include "pulse/models/reelcomment.hpp"
#include "pulse/models/snap.hpp"
#include "pulse/models/bookmark.hpp"
#include "pulse/models/whisper.hpp"
#include "pulse/models/pulsedrop.hpp"
#include "pulse/models/chainstory.hpp"
#include "pulse/models/alterego.hpp"
#include "pulse/models/socialdna.hpp"
#include "pulse/models/pulsescore.hpp"
#include "pulse/models/roulette.hpp"
#include "pulse/models/userbehavior.hpp"
#include "pulse/models/userengagement.hpp"
