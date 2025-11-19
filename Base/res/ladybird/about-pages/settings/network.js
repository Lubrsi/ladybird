const customDnsSettings = document.querySelector("#custom-dns-settings");
const dnsForciblyEnabled = document.querySelector("#dns-forcibly-enabled");

const dnsUpstream = document.querySelector("#dns-upstream");
const dnsType = document.querySelector("#dns-type");
const dnsServer = document.querySelector("#dns-server");
const dnsServerInvalidMessage = document.querySelector("#dns-server-invalid-message");
const dnsPort = document.querySelector("#dns-port");
const dnsPortGroup = document.querySelector("#dns-port-group");
const dnssecToggle = document.querySelector("#dnssec-toggle");

let DNS_SETTINGS = {};

function loadSettings(settings) {
    DNS_SETTINGS = settings.dnsSettings || {};
    loadDnsSettings();
}

function loadDnsSettings() {
    dnsUpstream.value = DNS_SETTINGS.mode || "system";

    if (dnsUpstream.value === "custom") {
        dnsType.value = DNS_SETTINGS.type;
        dnsServer.value = DNS_SETTINGS.server;
        dnsPort.value = DNS_SETTINGS.port;
        dnssecToggle.checked = DNS_SETTINGS.dnssec;

        if (dnsType.value === "https") {
            dnsPortGroup.classList.add("hidden");
        } else {
            dnsPortGroup.classList.remove("hidden");
        }

        customDnsSettings.classList.remove("hidden");
    } else {
        dnsType.value = "udp";
        dnsServer.value = "";
        dnsPort.value = "53";
        dnssecToggle.checked = false;

        customDnsSettings.classList.add("hidden");
    }

    if (DNS_SETTINGS.forciblyEnabled) {
        dnsForciblyEnabled.classList.remove("hidden");

        dnsUpstream.disabled = true;
        dnsType.disabled = true;
        dnsServer.disabled = true;
        dnsPort.disabled = true;
        dnssecToggle.disabled = true;
    } else {
        dnsForciblyEnabled.classList.add("hidden");

        dnsUpstream.disabled = false;
        dnsType.disabled = false;
        dnsServer.disabled = false;
        dnsPort.disabled = false;
        dnssecToggle.disabled = false;
    }
}

dnsUpstream.addEventListener("change", () => {
    if (dnsUpstream.value === "custom") {
        customDnsSettings.classList.remove("hidden");

        if (dnsServer.value.length !== 0 && dnsPort.value.length !== 0) {
            updateDnsSettings();
        }
    } else {
        customDnsSettings.classList.add("hidden");
        ladybird.sendMessage("setDNSSettings", { mode: "system" });
    }
});

function updateDnsSettings() {
    if (dnsUpstream.value !== "custom") {
        return;
    }

    dnsPort.placeholder = dnsType.value === "tls" ? "853" : "53";

    if ((dnsPort.value || 0) === 0) {
        dnsPort.value = dnsPort.placeholder;
    }

    const type = dnsType.value;

    let server;

    if (type !== "https") {
        server = dnsServer.value;
        dnsPortGroup.classList.remove("hidden");
    } else {
        dnsPortGroup.classList.add("hidden");

        const showInvalidURLMessage = (reason) => {
            dnsServer.classList.add("invalid");
            dnsServerInvalidMessage.classList.remove("hidden");
            dnsServerInvalidMessage.innerText = `Unable to use this resolver URL: ${reason}`;
        };
        
        const resolverUrl = URL.parse(dnsServer.value);
        if (resolverUrl === null) {
            showInvalidURLMessage("Invalid URL");
            return;
        }

        if (resolverUrl.protocol !== "https:") {
            showInvalidURLMessage("URL must use the HTTPS protocol");
            return;
        }

        if (resolverUrl.hostname === "") {
            showInvalidURLMessage("URL must have a hostname");
            return;
        }

        if (resolverUrl.username !== "" || resolverUrl.password !== "" || resolverUrl.search !== "" || resolverUrl.hash !== "") {
            showInvalidURLMessage("URL can only have a protocol, hostname, port and path");
            return;
        }

        server = resolverUrl.href;
        dnsServer.classList.remove("invalid");
        dnsServerInvalidMessage.classList.add("hidden");
    }

    ladybird.sendMessage("setDNSSettings", {
        mode: "custom",
        type,
        server,
        port: dnsPort.value | 0,
        dnssec: dnssecToggle.checked,
    });
}

dnsServer.addEventListener("change", updateDnsSettings);
dnsPort.addEventListener("change", updateDnsSettings);
dnsType.addEventListener("change", updateDnsSettings);
dnssecToggle.addEventListener("change", updateDnsSettings);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
