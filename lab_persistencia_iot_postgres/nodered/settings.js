module.exports = {
    flowFile: "flows.json",
    flowFilePretty: true,
    uiPort: process.env.PORT || 1880,
    mqttReconnectTime: 15000,
    serialReconnectTime: 15000,
    debugMaxLength: 1000,
    editorTheme: {
        projects: {
            enabled: true
        }
    },
    contextStorage: {
        default: "memory",
        memory: { module: "memory" }
    },
    logging: {
        console: {
            level: "info",
            metrics: false,
            audit: false
        }
    },
    credentialSecret: false,
    adminAuth: null,
    functionExternalModules: true,
    httpAdminRoot: '/',
    httpNodeRoot: '/api',
    nodesDir: null
};
