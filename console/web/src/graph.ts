import type {Node, Edge} from '@xyflow/react';
export type Runtime = {state: string; observedAt: string | null; mappingEpoch: string | null; identity: unknown; reason: string};
export type Inventory = {
  topologyHash: string; topologyId: string; generatedAt: string; runtimeObservedAt: string | null; runtimeFreshness: string;
  nodes: {id: string; kind: string; configuration: Record<string, unknown>; runtime: Runtime; failureDomain: string | null}[];
  edges: {id: string; endpoints: [string, string]; runtime: Runtime; adminState: string; carrierState: string; rstpState: string; captureState: string; rates: Record<string, number | null>}[];
  management: {networks: Record<string, unknown>}; managementAttachments: {endpoint: string; network: string; address: string}[];
  failureDomains: {id: string; state: string; description: string}[];
};
export function graph(inventory: Inventory, management: boolean): {nodes: Node[]; edges: Edge[]} {
  const columns = Math.max(1, Math.ceil(Math.sqrt(inventory.nodes.length)));
  const nodes: Node[] = inventory.nodes.map((node, i) => ({id: node.id, type: 'logical', position: {x: (i % columns) * 260, y: Math.floor(i / columns) * 190},
    data: {label: node.id, kind: node.kind, state: node.runtime.state, domain: node.failureDomain}}));
  const pairs = new Map<string, string[]>();
  for (const edge of inventory.edges) {
    const key = edge.endpoints.map(e => e.split(':')[0]).sort().join(':');
    pairs.set(key, [...(pairs.get(key) ?? []), edge.id]);
  }
  const edges: Edge[] = inventory.edges.map(edge => {
    const [source, target] = edge.endpoints.map(e => e.split(':')[0]);
    const group = pairs.get([source, target].sort().join(':'))!;
    const offset = (group.indexOf(edge.id) - (group.length - 1) / 2) * 65;
    return {id: edge.id, source, target, type: 'cable', data: {offset, label: edge.id}, ariaLabel: `Data edge ${edge.id}: ${edge.endpoints.join(' to ')}; runtime ${edge.runtime?.state ?? 'unknown'}`};
  });
  if (management) {
    Object.keys(inventory.management.networks).forEach((id, i) => nodes.push({id: `management/${id}`, type: 'logical',
      position: {x: i * 260, y: Math.ceil(inventory.nodes.length / columns) * 190}, data: {label: id, kind: 'management', state: 'unknown'}}));
    inventory.managementAttachments.forEach(a => edges.push({id: `management/${a.endpoint}`, source: a.endpoint.split(':')[0], target: `management/${a.network}`,
      type: 'cable', data: {offset: 0, label: a.address, management: true}, style: {strokeDasharray: '6 6', stroke: '#b986d8'}, ariaLabel: `Management attachment ${a.endpoint}`}));
  }
  return {nodes, edges};
}
