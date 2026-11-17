#pragma once
// Rack side of GridController.hpp: one gridctl::Port per JAM module, between its MIDI ports and its JAM code.
//   while (ctl.pop(midiIn, midiOut, &msg, args.frame, padNote(0, 0), jam.active)) { ...the JAM code as before... }
//   ctl.send(midiOut, m, padNote(0, 0));        // instead of midiOut.sendMessage(m) for the lights
//   ctl.toJson(root) / ctl.fromJson(root);      // "controller": the menu choice (missing = Maschine JAM)
//   ctl.appendMenu(menu);                       // Controller: Auto / Maschine JAM / APC mini mk2 (beta) / Launchpad ...
// With the Maschine JAM in use, pop() is midiIn.tryPop() and send() is midiOut.sendMessage(): the same calls and
// bytes as before the profiles existed. GPL-3.0-or-later like the free singles.
#include <rack.hpp>
#include "GridController.hpp"

namespace gridctl {

struct Port {
    int sel = AUTO;      // menu choice: AUTO or a Kind (new modules: AUTO; patches saved before this: JAM)
    int kind = JAM;      // the profile in use
    Translator tr;
    bool connected = false, wasActive = false;
    int lastSel = -99, lastIn = -99, lastOut = -99, lastInDrv = -99, lastOutDrv = -99;

    // pick the profile again when the choice or a MIDI device changes (cheap: compares ids each call)
    // (in = NULL: from send(), which only has the output port; the input keeps what pop() saw last)
    void resolve(rack::midi::Port* in, rack::midi::Port& out) {
        int i = in ? in->getDeviceId() : lastIn, o = out.getDeviceId(), id = in ? in->getDriverId() : lastInDrv, od = out.getDriverId(), s = sel;
        if (s == lastSel && i == lastIn && o == lastOut && id == lastInDrv && od == lastOutDrv) return;
        lastSel = s; lastIn = i; lastOut = o; lastInDrv = id; lastOutDrv = od;
        int k = s;
        if (k == AUTO) {
            k = -1;
            if (o >= 0) k = kindFromDeviceName(out.getDeviceName(o));   // the lights' port decides
            if (k < 0 && in && i >= 0) k = kindFromDeviceName(in->getDeviceName(i));
            if (k < 0) k = JAM;
        }
        if (k < 0 || k >= NUM_KINDS) k = JAM;
        kind = k;
        tr.kind = k;
        tr.reset();
        connected = false;   // a new device (or profile) gets its connect messages again
    }

    void sendDevice(rack::midi::Output& out, const MsgList& l) {
        if (!l.n) return;
        int ch = out.channel;
        out.channel = -1;   // the APC's LED brightness is the MIDI channel: don't let the port overwrite it
        for (int i = 0; i < l.n; i++) {
            rack::midi::Message m;
            m.bytes.assign(l.m[i].b, l.m[i].b + l.m[i].n);
            out.sendMessage(m);
        }
        out.channel = ch;
    }

    // Next JAM-space message for the module, or false. `active` = this module owns the controller (JamOwner):
    // only the owner repaints the SHIFT layer; gaining ownership sends the connect messages again.
    bool pop(rack::midi::InputQueue& in, rack::midi::Output& out, rack::midi::Message* msg, int64_t frame, int padBase, bool active) {
        resolve(&in, out);
        if (kind == JAM) return in.tryPop(msg, frame);
        if (active && !wasActive) { connected = false; tr.reset(); }
        wasActive = active;
        rack::midi::Message raw;
        while (in.tryPop(&raw, frame)) {
            Msg j;
            MsgList rep;
            bool ok = tr.toJam(raw.bytes.data(), (int)raw.bytes.size(), padBase, &j, &rep);
            if (active) sendDevice(out, rep);
            if (ok) {
                msg->bytes.assign(j.b, j.b + j.n);
                msg->frame = raw.frame;
                return true;
            }
        }
        return false;
    }

    // a module's light (JAM-space) -> the controller
    void send(rack::midi::Output& out, const rack::midi::Message& m, int padBase) {
        resolve(NULL, out);
        if (kind == JAM) { out.sendMessage(m); return; }   // unchanged JAM path
        MsgList l;
        if (!connected) { connectMsgs(kind, &l); connected = true; }
        tr.ledToDevice(m.bytes.data(), (int)m.bytes.size(), padBase, &l);
        sendDevice(out, l);
    }

    void toJson(json_t* root) { json_object_set_new(root, "controller", json_string(kindId(sel))); }
    void fromJson(json_t* root) {
        json_t* j = json_object_get(root, "controller");
        sel = j ? kindFromId(json_string_value(j)) : JAM;   // saved before controller profiles = the JAM
    }

    void appendMenu(rack::ui::Menu* menu) {
        Port* p = this;
        menu->addChild(rack::createSubmenuItem("Controller", kindLabel(sel), [=](rack::ui::Menu* sub) {
            for (int k = AUTO; k < NUM_KINDS; k++)
                sub->addChild(rack::createCheckMenuItem(kindLabel(k), "", [=]() { return p->sel == k; }, [=]() { p->sel = k; }));
            sub->addChild(new rack::ui::MenuSeparator);
            sub->addChild(rack::createMenuLabel(std::string("In use: ") + kProfiles[p->kind].label));
            sub->addChild(rack::createMenuLabel(std::string("MIDI port: ") + kProfiles[p->kind].portHint));
            sub->addChild(rack::createMenuLabel(std::string("PLAY: ") + kProfiles[p->kind].playHint + " (with a COMPOSER: the song)"));
            if (p->kind != JAM) sub->addChild(rack::createMenuLabel("One controller at a time: the last clicked module owns it"));
            if (p->kind != JAM) sub->addChild(rack::createMenuLabel("beta: not yet tried on this controller"));
        }));
    }
};

}  // namespace gridctl
