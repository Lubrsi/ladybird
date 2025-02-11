const consoleMethods = [
    "clear",
    "debug",
    "error",
    "info",
    "log",
    "table",
    "trace",
    "warn",
    "dir",
    "dirxml",
    "count",
    "countReset",
    "group",
    "groupCollapsed",
    "groupEnd",
];

const error = new Error("blah");
Object.defineProperty(error, "name", { 
    get() {
        console.log("got name!");
        return "namesuccess";
    }
});
Object.defineProperty(error, "message", { 
    get() {
        console.log("got message!");
        return "messagesuccess";
    }
});
Object.defineProperty(error, "stack", { 
    get() {
        console.log("got stack!");
        return "stacksuccess";
    }
});

for (const method of consoleMethods) {
    console.log("== begin", method);
    const valueToPrint = method === "table" ? [error] : error;
    console[method]("", valueToPrint);
    console.log("== end", method);
}