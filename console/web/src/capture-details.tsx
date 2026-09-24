import React from "react";
export function CaptureDetails({artifact:a}:{artifact:any}) {
 if(a.mediaType==="application/x-graphlab-terminal")return null;
 const show=(v:any)=>v==null?"Unavailable":String(v);
 return <details><summary>Capture metadata {a.id ?? a.captureId}</summary>
 <p>{a.provenance ?? "Legacy or incomplete capture: metadata availability is limited."}</p>
 <dl>
 <dt>Capture node (declared)</dt><dd>{a.canonicalEndpoint?.split(":")[0] ?? "Unavailable"}</dd>
 <dt>Capture point (declared endpoint)</dt><dd>{show(a.canonicalEndpoint)}</dd>
 <dt>Runtime capture interface</dt><dd>{show(a.captureInterface)}</dd>
 <dt>File format / link type</dt><dd>{show(a.format)} / {a.linkType===1?"1 · Ethernet":show(a.linkType)}</dd>
 <dt>Finalized at (worker wall clock)</dt><dd>{show(a.closedAt)}</dd>
 <dt>Mapping identity digest</dt><dd>{show(a.mappingEpoch)}</dd>
 <dt>Run capture coverage</dt><dd>{show(a.captureCoverage)}</dd>
 <dt>Packets in segment</dt><dd>{show(a.packets)}</dd>
 <dt>Captured / original packet bytes</dt><dd>{show(a.packetLengths?.capturedBytes)} / {show(a.packetLengths?.originalBytes)}</dd>
 <dt>Truncated packets</dt><dd>{show(a.packetLengths?.truncatedPackets)}</dd>
 <dt>Snapshot length limit</dt><dd>{show(a.limits?.snaplen)} bytes</dd>
 <dt>Capture byte budget / rotation threshold</dt><dd>{show(a.limits?.byteBudget)} / {show(a.limits?.rotateBytes)} bytes</dd>
 <dt>Rotation interval</dt><dd>{show(a.limits?.rotateSeconds)} seconds</dd>
 <dt>SHA-256 from finalized manifest</dt><dd>{show(a.sha256)}</dd>
 </dl><p>Length totals count packet bytes, excluding PCAPNG overhead. Missing summaries are unavailable, not zero. Capture coverage does not prove delivery; packet direction and application-message correlation remain unavailable. Downloads verify retained bytes against the manifest checksum.</p>
 </details>;
}
