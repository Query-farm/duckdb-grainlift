// Application-owned Iroh adapter Worker (Haybarn `irohAdapterWorker`): one
// browser Iroh node serving the grainlift extension's iroh:// SAB rings.
import { createIrohNode } from './iroh/index.js';
import { installIrohVgiAdapter } from './iroh/adapter-worker.js';

const secretKey = new URL(self.location.href).searchParams.get('key') || undefined;
const nodePromise = createIrohNode({ secretKey });
nodePromise.then(
    (node) => self.postMessage({ type: 'grainlift-iroh-node', endpointId: node.endpointId }),
    (error) => self.postMessage({ type: 'grainlift-iroh-node', error: String(error) }),
);
installIrohVgiAdapter(nodePromise);
