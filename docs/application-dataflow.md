# Application dataflow mapping v1

Topologies may include an optional `application` object. This is declarative metadata in `graphlab.topology/v2`; it does not start workloads, change forwarding, generate traffic, or alter capture policy.

```yaml
application:
  apiVersion: graphlab.application-dataflow/v1
  edges:
    - id: requests
      source: client
      target: server
      networkEdges: [client-switch, switch-server]
    - id: replies
      source: server
      target: client
      networkEdges: [client-switch, switch-server]
```

`source` and `target` are existing Docker or QEMU topology node IDs. Each workload node represents one application participant in this increment. An application edge is directed and has its own unique identifier; its ID namespace is independent of network edge IDs. Parallel, reverse, and self relationships are allowed. The example requires the named workloads and data links to exist in the enclosing topology.

`networkEdges` is a required, unordered set of existing **data edge** IDs. It declares associated network resources; it is neither a routed path nor proof that traffic traversed those resources. Empty means explicitly unspecified. Multiple application edges may share a network edge. Disconnected resource sets are allowed because the declaration makes no reachability claim. There is no inferred routing, bandwidth allocation, exclusivity, interface direction, message identity, delivery loss, latency, or application-to-packet correlation. Management attachments cannot be referenced. Interface details and capture references remain available through the selected network edge.

Validation rejects unknown fields/versions, missing or switch endpoints, duplicate application IDs, duplicate resource references, and unknown network edges. Identifiers follow the existing 64-character topology identifier rule. Limits are 1,024 application edges and 256 network references per application edge, subject also to existing document/catalog byte limits. Application edges and resource sets are sorted during canonicalization. Changes affect the immutable topology hash; input ordering does not. Topologies without the object retain their existing canonical form and hash.

The existing authenticated inventory endpoint exposes `application` as the canonical object, or `null` for legacy topologies. No new unauthenticated surface, runtime control, or persistence database is introduced. Metadata is stored with the topology revision and run topology snapshot. Earlier binaries that reject unknown topology fields need upgrading before loading annotated revisions.

The console's **Topology view** selector switches between network resources and application dataflow. The application view shows workload nodes and declared directed edges, without network-rate badges. The mapping inventory supports keyboard navigation, source/target workload inspection, and navigation to individual associated network edges. Selecting a network edge identifies the application declarations that reference it; selecting an application edge highlights its associated network links. Existing network inspectors, packet filters, capture catalog, and node consoles retain their network/workload scopes. Missing declarations and empty mappings are labeled explicitly.

The subsequent [application-edge telemetry extension](application-edge-telemetry.md) adds optional declared protocol metadata and endpoint-owned reports for multiple declared edges on a workload. Multiple application components within one workload, observed route changes and control of individual sources remain unsupported. Associations are operator declarations and are not independently verified against runtime traffic.
