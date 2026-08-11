// Example pentagotchi plugin (pwnagotchi-compatible API).
//
// Drop this file into /plugins/ on the SD card. The firmware loads every
// *.js it finds there and calls the on_* hooks wired to firmware events.
//
// Plugins are disabled by default. Flip "enabled" to true in /plugins.json
// (created automatically at the SD root) to activate this one:
//     { "plugins": { "example": { "enabled": true, ... } } }
//
// __defaults__.options are seeded into /plugins.json the first time the
// plugin is discovered; from then on the config file is the source of truth
// and the plugin reads them through this.options.*.

var Example = new Plugin({
    __author__: 'pentagotchi',
    __version__: '0.1.0',
    __license__: 'GPL3',
    __description__: 'example plugin: logs events and pokes the UI',

    __defaults__: {
        options: {
            log_level: 1,
            announce_events: false,
        },
    },

    on_loaded: function(agent) {
        console.log('[example] loaded, agent =', agent);
        console.log('[example] options =', JSON.stringify(this.options));
    },

    on_ui_setup: function(ui) {
        console.log('[example] ui setup');
    },

    on_ui_update: function(ui) {
        // ui is the 'ui' global: ui.get(key) / ui.status
        var st = ui.get('status');
        if (st && st !== ui.last_status) {
            console.log('[example] status =', st);
            ui.last_status = st;
        }
    },

    on_ready: function(agent) {
        console.log('[example] ready, identity =', agent.identity());
        agent.set_face(faces.HAPPY);
        agent.set_status('example plugin ready');
    },

    on_epoch: function(agent, epoch) {
        console.log('[example] epoch', epoch, 'channel', ui.get('channel'));
    },

    on_channel_hop: function(agent, channel) {
        console.log('[example] hop -> ch', channel);
    },

    on_ap_detected: function(agent, ap) {
        console.log('[example] AP', ap.ssid, ap.bssid, 'ch', ap.channel, 'rssi', ap.rssi);
    },

    on_deauthentication: function(agent, ap, client) {
        console.log('[example] deauth', client.mac);
    },

    on_handshake: function(agent, filename, ap, client) {
        console.log('[example] handshake saved ->', filename);
        agent.set_face(faces.EXCITED);
        agent.set_status('pwnd! ' + filename);
    },

    on_peer_detected: function(agent, peer) {
        console.log('[example] peer detected:', peer.identity);
    },

    on_peer_encounter: function(agent, peer) {
        console.log('[example] peer encounter #', peer.encounters);
    },

    on_peer_lost: function(agent, peer) {
        console.log('[example] peer lost:', peer.identity);
    },

    on_friend: function(agent, peer) {
        console.log('[example] friend!', peer.identity);
        agent.set_face(faces.FRIEND);
    },
});
