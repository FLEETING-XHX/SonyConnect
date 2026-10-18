'use client';
import { useState } from 'react';
import SquishSwitch from './SquishSwitch';

export default function Example() {
  const [enabled, setEnabled] = useState(false);
  return (
    <SquishSwitch
      checked={enabled}
      onChange={setEnabled}
      label="功能开关"
      trackColor="#D9DEEA"
      trackOnColor="#4C5ED6"
      thumbColor="#FFFFFF"
      thumbOnColor="#FFFFFF"
      width={76}
      height={38}
      radius={19}
      duration={220}
      colorDuration={180}
      draggable
      disabled={false}
    />
  );
}
