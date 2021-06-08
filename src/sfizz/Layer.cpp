// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Layer.h"
#include "Region.h"
#include "utility/Debug.h"
#include "utility/SwapAndPop.h"
#include <absl/algorithm/container.h>

namespace sfz {

Layer::Layer(int regionNumber, absl::string_view defaultPath, const MidiState& midiState)
    : midiState_(midiState), region_(regionNumber, defaultPath)
{
    initializeActivations();
}

Layer::Layer(const Region& region, const MidiState& midiState)
    : midiState_(midiState), region_(region)
{
    initializeActivations();
}

Layer::~Layer()
{
}

void Layer::initializeActivations()
{
    const Region& region = region_;

    keySwitched_ = !region.usesKeySwitches;
    previousKeySwitched_ = !region.usesPreviousKeySwitches;
    sequenceSwitched_ = !region.usesSequenceSwitches;
    pitchSwitched_ = true;
    bpmSwitched_ = true;
    aftertouchSwitched_ = true;
    ccSwitched_.set();
}

bool Layer::isSwitchedOn() const noexcept
{
    return keySwitched_ && previousKeySwitched_ && sequenceSwitched_ && pitchSwitched_ && bpmSwitched_ && aftertouchSwitched_ && ccSwitched_.all();
}

void Layer::walkSequence() noexcept
{
    sequenceSwitched_ =
        ((sequenceCounter_++ % region_.sequenceLength) == region_.sequencePosition - 1); 
}

bool Layer::checkRandom(float randValue) const noexcept
{
    return region_.randRange.contains(randValue) || (randValue >= 1.0f && region_.randRange.isValid() && region_.randRange.getEnd() >= 1.0f);
}

bool Layer::checkNote(int noteNumber, float velocity) const noexcept
{
    return region_.keyRange.containsWithEnd(noteNumber) && region_.velocityRange.containsWithEnd(velocity);
}

bool Layer::registerNoteOn(int noteNumber, float velocity, float randValue) noexcept
{
    ASSERT(velocity >= 0.0f && velocity <= 1.0f);

    const Region& region = region_;

    if (region.velocityOverride == VelocityOverride::previous)
        velocity = midiState_.getVelocityOverride();

    if ( !(region.triggerOnNote && checkNote(noteNumber, velocity) && checkRandom(randValue)) ) {
        return false;
    }

    const bool polyAftertouchActive =
        region.polyAftertouchRange.containsWithEnd(midiState_.getPolyAftertouch(noteNumber));

    if (!polyAftertouchActive) {
        return false;
    }

    const bool firstLegatoNote = (region.trigger == Trigger::first && midiState_.getActiveNotes() == 1);
    const bool attackTrigger = (region.trigger == Trigger::attack);
    const bool notFirstLegatoNote = (region.trigger == Trigger::legato && midiState_.getActiveNotes() > 1);

    if (attackTrigger || firstLegatoNote || notFirstLegatoNote) {
        walkSequence();
        return isSwitchedOn();
    }

    return false;
}

bool Layer::registerNoteOff(int noteNumber, float velocity, float randValue) noexcept
{
    ASSERT(velocity >= 0.0f && velocity <= 1.0f);

    const Region& region = region_;

    if (region.velocityOverride == VelocityOverride::previous)
        velocity = midiState_.getVelocityOverride();

    if (!(region.triggerOnNote && checkNote(noteNumber, velocity) && checkRandom(randValue))) {
        return false;
    }

    const bool polyAftertouchActive =
        region.polyAftertouchRange.containsWithEnd(midiState_.getPolyAftertouch(noteNumber));

    // Prerequisites

    if (!polyAftertouchActive)
        return false;

    // Release logic

    bool triggerOk = false;

    if (region.trigger == Trigger::release_key) {
        triggerOk = true;
    } else if (region.trigger == Trigger::release) {
        const bool sostenutoed = isNoteSostenutoed(noteNumber);

        if (sostenutoed && !sostenutoPressed_) {
            removeFromSostenutoReleases(noteNumber);
            if (sustainPressed_)
                delaySustainRelease(noteNumber, midiState_.getNoteVelocity(noteNumber));
        }

        if (!sostenutoPressed_ || !sostenutoed) {
            if (sustainPressed_)
                delaySustainRelease(noteNumber, midiState_.getNoteVelocity(noteNumber));
            else
                triggerOk = true;
        }
    }

    if (triggerOk) {
        walkSequence();
        return isSwitchedOn();
    }

    return false;
}

bool Layer::registerCC(int ccNumber, float ccValue, float randValue) noexcept
{
    const Region& region = region_;

    if (ccNumber == region.sustainCC)
        sustainPressed_ = region.checkSustain && ccValue >= region.sustainThreshold;

    if (ccNumber == region.sostenutoCC) {
        const bool newState = region.checkSostenuto && ccValue >= region.sostenutoThreshold;
        if (!sostenutoPressed_ && newState)
            storeSostenutoNotes();

        if (!newState && sostenutoPressed_)
            delayedSostenutoReleases_.clear();

        sostenutoPressed_ = newState;
    }

    if (region.ccConditions.getWithDefault(ccNumber).containsWithEnd(ccValue))
        ccSwitched_.set(ccNumber, true);
    else
        ccSwitched_.set(ccNumber, false);

    if (!region.triggerOnCC)
        return false;

    if (auto triggerRange = region.ccTriggers.get(ccNumber)) {
        if (triggerRange->containsWithEnd(ccValue)) {
            walkSequence();
            return isSwitchedOn();
        }
    }

    return false;
}

void Layer::registerPitchWheel(float pitch) noexcept
{
    const Region& region = region_;
    if (region.bendRange.containsWithEnd(pitch))
        pitchSwitched_ = true;
    else
        pitchSwitched_ = false;
}

void Layer::registerAftertouch(float aftertouch) noexcept
{
    const Region& region = region_;
    if (region.aftertouchRange.containsWithEnd(aftertouch))
        aftertouchSwitched_ = true;
    else
        aftertouchSwitched_ = false;
}

void Layer::registerTempo(float secondsPerQuarter) noexcept
{
    const Region& region = region_;
    const float bpm = 60.0f / secondsPerQuarter;
    if (region.bpmRange.containsWithEnd(bpm))
        bpmSwitched_ = true;
    else
        bpmSwitched_ = false;
}

void Layer::delaySustainRelease(int noteNumber, float velocity) noexcept
{
    if (delayedSustainReleases_.size() == delayedSustainReleases_.capacity())
        return;

    delayedSustainReleases_.emplace_back(noteNumber, velocity);
}

void Layer::delaySostenutoRelease(int noteNumber, float velocity) noexcept
{
    if (delayedSostenutoReleases_.size() == delayedSostenutoReleases_.capacity())
        return;

    delayedSostenutoReleases_.emplace_back(noteNumber, velocity);
}

void Layer::removeFromSostenutoReleases(int noteNumber) noexcept
{
    swapAndPopFirst(delayedSostenutoReleases_, [=](const std::pair<int, float>& p) {
        return p.first == noteNumber;
    });
}

void Layer::storeSostenutoNotes() noexcept
{
    ASSERT(delayedSostenutoReleases_.empty());
    const Region& region = region_;
    for (int note = region.keyRange.getStart(); note <= region.keyRange.getEnd(); ++note) {
        if (midiState_.isNotePressed(note))
            delaySostenutoRelease(note, midiState_.getNoteVelocity(note));
    }
}


bool Layer::isNoteSustained(int noteNumber) const noexcept
{
    return absl::c_find_if(delayedSustainReleases_, [=](const std::pair<int, float>& p) {
        return p.first == noteNumber;
    }) != delayedSustainReleases_.end();
}

bool Layer::isNoteSostenutoed(int noteNumber) const noexcept
{
    return absl::c_find_if(delayedSostenutoReleases_, [=](const std::pair<int, float>& p) {
        return p.first == noteNumber;
    }) != delayedSostenutoReleases_.end();
}

} // namespace sfz
