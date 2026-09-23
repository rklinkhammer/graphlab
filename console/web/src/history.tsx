import React, { useState } from "react";
type Event = { at: string; kind: string; detail: unknown };
export function HistoryPanel({ events }: { events: Event[] }) {
  const [frozen, setFrozen] = useState<Event[] | null>(null),
    [count, setCount] = useState(100),
    [filter, setFilter] = useState("");
  const source = frozen ?? events,
    rows = source
      .filter(
        (e) =>
          !filter ||
          JSON.stringify(e).toLowerCase().includes(filter.toLowerCase()),
      )
      .slice()
      .reverse();
  return (
    <section aria-label="Event history">
      <h3>Event history</h3>
      <p>
        {source.length} returned events ·{" "}
        {frozen ? "Paused while browsing older records" : "Following newest"} ·
        bounded server retention; older events may no longer exist.
      </p>
      <label>
        Filter history{" "}
        <input
          value={filter}
          onChange={(e) => {
            setFilter(e.target.value);
            setCount(100);
          }}
        />
      </label>
      <ol>
        {rows.slice(0, count).map((e, i) => (
          <li key={`${e.at}-${i}`}>
            <strong>
              {e.at} · {e.kind}
            </strong>
            <pre>{JSON.stringify(e.detail, null, 2)}</pre>
          </li>
        ))}
      </ol>
      {!rows.length && <p>No matching retained events.</p>}
      <button
        disabled={count >= rows.length}
        onClick={() => {
          setFrozen(source.slice());
          setCount((n) => n + 100);
        }}
      >
        Load older records
      </button>
      <button
        disabled={!frozen}
        onClick={() => {
          setFrozen(null);
          setCount(100);
        }}
      >
        Return to newest
      </button>
      <p>
        Packet/message history is unavailable: this timeline records operations
        and observations, not individual packets.
      </p>
    </section>
  );
}
