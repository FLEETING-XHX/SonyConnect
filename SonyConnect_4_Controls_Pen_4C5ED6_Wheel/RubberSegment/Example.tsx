import RubberSegment from './RubberSegment';
import type { ReactNode } from 'react';

// Minimal inline SVGs avoid adding an icon library dependency.
function Headphones(): ReactNode {
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M3 14v-3a9 9 0 0 1 18 0v3"/><rect x="3" y="13" width="4" height="8" rx="2"/><rect x="17" y="13" width="4" height="8" rx="2"/></svg>;
}
function Ear(): ReactNode {
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M6 11a6 6 0 1 1 12 0c0 3.3-2 4.5-4 6.5-.9.9-1 2.5-3 2.5-1.7 0-3-1.3-3-3"/><path d="M9 11a3 3 0 0 1 6 0c0 1.7-1 2.8-2.4 3.8"/></svg>;
}
function Power(): ReactNode {
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M12 2v10"/><path d="M7 5.5a9 9 0 1 0 10 0"/></svg>;
}

export default function Example() {
  return (
    <RubberSegment
      items={[
        { value: 'anc', label: '降噪', icon: <Headphones /> },
        { value: 'ambient', label: '通透', icon: <Ear /> },
        { value: 'off', label: '关闭', icon: <Power /> }
      ]}
      defaultValue="anc"
      onChange={(value, index) => console.log(value, index)}
      trackColor="#edeff5"
      thumbColor="#ffffff"
      textColor="#626b7e"
      activeTextColor="#4C5ED6"
      size="lg"
      radius={14}
      inset={5}
      equalSlots
      duration={220}
      disabled={false}
    />
  );
}
