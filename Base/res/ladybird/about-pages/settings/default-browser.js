const status = document.querySelector("#default-browser-status");
const button = document.querySelector("#default-browser-set");

const applyStatus = ({ supported, isDefault }) => {
    if (!supported) {
        status.textContent = "Ladybird can't change your default browser on this system.";
        button.classList.add("hidden");
        return;
    }

    if (isDefault) {
        status.textContent = "Ladybird is your default browser.";
        button.classList.add("hidden");
    } else {
        status.textContent = "Ladybird is not your default browser.";
        button.classList.remove("hidden");
    }
};

button.addEventListener("click", () => {
    ladybird.sendMessage("setAsDefaultBrowser");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "defaultBrowserStatus") {
        applyStatus(event.detail.data);
    }
});
