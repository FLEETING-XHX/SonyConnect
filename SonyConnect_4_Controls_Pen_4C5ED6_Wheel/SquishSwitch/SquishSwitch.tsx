'use client';

import React, {
  useEffect,
  useId,
  useRef,
  useState,
  type CSSProperties,
  type KeyboardEvent,
  type PointerEvent
} from 'react';
import { animate, motion, useMotionValue, useReducedMotion } from 'motion/react';
import './SquishSwitch.css';

export interface SquishSwitchProps {
  checked?: boolean;
  defaultChecked?: boolean;
  onChange?: (checked: boolean) => void;
  label?: string;
  disabled?: boolean;
  trackColor?: string;
  trackOnColor?: string;
  thumbColor?: string;
  thumbOnColor?: string;
  width?: number;
  height?: number;
  radius?: number;
  duration?: number;
  draggable?: boolean;
  colorDuration?: number;
  ariaLabel?: string;
  className?: string;
  id?: string;
}

type Grip = {
  id: number;
  originX: number;
  originThumbX: number;
  moved: boolean;
  slop: number;
};

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));
const EASE_OUT: [number, number, number, number] = [0.23, 1, 0.32, 1];

/** Smooth, non-elastic version of React Bits Squish Switch. */
const SquishSwitch: React.FC<SquishSwitchProps> = ({
  checked,
  defaultChecked = false,
  onChange,
  label = '',
  disabled = false,
  trackColor = '#D9DEEA',
  trackOnColor = '#4C5ED6',
  thumbColor = '#FFFFFF',
  thumbOnColor = '#FFFFFF',
  width = 76,
  height = 38,
  radius = 19,
  duration = 220,
  draggable = true,
  colorDuration = 180,
  ariaLabel,
  className = '',
  id
}) => {
  const reduceMotion = useReducedMotion();
  const inset = Math.max(3, Math.round(height * 0.11));
  const thumb = Math.max(1, height - inset * 2);
  const minX = inset;
  const maxX = Math.max(minX, width - inset - thumb);
  const midX = (minX + maxX) / 2;
  const trackRadius = Math.min(radius, height / 2);
  const thumbRadius = Math.max(2, trackRadius - inset);

  const controlled = checked !== undefined;
  const [inner, setInner] = useState(defaultChecked);
  const on = controlled ? checked : inner;
  const onRef = useRef(on);
  onRef.current = on;
  const [dragging, setDragging] = useState(false);
  const draggingRef = useRef(false);
  const grip = useRef<Grip | null>(null);
  const suppressClick = useRef(false);
  const currentAnimation = useRef<ReturnType<typeof animate> | null>(null);
  const buttonRef = useRef<HTMLButtonElement>(null);
  const trackRef = useRef<HTMLSpanElement>(null);
  const autoId = useId();
  const buttonId = id ?? autoId;
  const x = useMotionValue(on ? maxX : minX);

  const commit = (next: boolean) => {
    if (disabled || next === onRef.current) return;
    onRef.current = next;
    if (!controlled) setInner(next);
    onChange?.(next);
  };

  useEffect(() => {
    if (dragging) return;
    const dest = on ? maxX : minX;
    currentAnimation.current?.stop();
    if (reduceMotion || duration <= 0) {
      x.set(dest);
      return;
    }
    // Tween, not spring: no bounce, deformation, or momentum.
    const controls = animate(x, dest, {
      type: 'tween',
      duration: Math.max(0, duration) / 1000,
      ease: EASE_OUT
    });
    currentAnimation.current = controls;
    return () => controls.stop();
  }, [on, dragging, minX, maxX, duration, reduceMotion, x]);

  const localX = (clientX: number) => {
    const el = trackRef.current;
    if (!el) return 0;
    const rect = el.getBoundingClientRect();
    const factor = rect.width / (el.offsetWidth || rect.width) || 1;
    return (clientX - rect.left) / factor;
  };

  const down = (e: PointerEvent<HTMLButtonElement>) => {
    if (disabled || grip.current || e.button !== 0) return;
    currentAnimation.current?.stop();
    grip.current = {
      id: e.pointerId,
      originX: localX(e.clientX),
      originThumbX: x.get(),
      moved: false,
      slop: e.pointerType === 'touch' ? 8 : 4
    };
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch { /* unavailable */ }
    draggingRef.current = true;
    setDragging(true);
  };

  const move = (e: PointerEvent<HTMLButtonElement>) => {
    const g = grip.current;
    if (!draggable || !g || e.pointerId !== g.id) return;
    const delta = localX(e.clientX) - g.originX;
    if (!g.moved && Math.abs(delta) <= g.slop) return;
    g.moved = true;
    x.set(clamp(g.originThumbX + delta, minX, maxX));
  };

  const up = (e: { pointerId: number; currentTarget: HTMLButtonElement }, cancelled: boolean) => {
    const g = grip.current;
    if (!g || e.pointerId !== g.id) return;
    grip.current = null;
    try { e.currentTarget.releasePointerCapture(e.pointerId); } catch { /* unavailable */ }
    draggingRef.current = false;
    if (!cancelled) {
      commit(g.moved ? x.get() >= midX : !onRef.current);
    }
    // Pointer handling already decided the state; ignore the following synthetic click.
    suppressClick.current = true;
    setTimeout(() => { suppressClick.current = false; }, 0);
    setDragging(false);
  };

  const click = () => {
    if (suppressClick.current) {
      suppressClick.current = false;
      return;
    }
    // Keyboard-generated clicks and label activation still work.
    commit(!onRef.current);
  };

  const keyDown = (e: KeyboardEvent<HTMLButtonElement>) => {
    if (e.key === 'Escape' && grip.current) {
      up({ pointerId: grip.current.id, currentTarget: e.currentTarget }, true);
    }
  };

  return (
    <span className={`squish-switch-root${className ? ` ${className}` : ''}`}>
      <button
        ref={buttonRef}
        id={buttonId}
        type="button"
        role="switch"
        aria-checked={on}
        aria-disabled={disabled || undefined}
        aria-label={ariaLabel || label || 'Switch'}
        disabled={disabled}
        className="squish-switch"
        data-on={on ? '' : undefined}
        data-held={dragging ? '' : undefined}
        style={{
          '--ss-w': `${width}px`,
          '--ss-h': `${height}px`,
          '--ss-inset': `${inset}px`,
          '--ss-thumb': `${thumb}px`,
          '--ss-r': `${trackRadius}px`,
          '--ss-thumb-r': `${thumbRadius}px`,
          '--ss-track': trackColor,
          '--ss-track-on': trackOnColor,
          '--ss-thumb-color': thumbColor,
          '--ss-thumb-on': thumbOnColor,
          '--ss-fade': `${reduceMotion ? 0 : colorDuration}ms`
        } as CSSProperties}
        onPointerDown={down}
        onPointerMove={move}
        onPointerUp={e => up(e, false)}
        onPointerCancel={e => up(e, true)}
        onLostPointerCapture={e => {
          if (grip.current) up({ pointerId: grip.current.id, currentTarget: e.currentTarget }, true);
        }}
        onKeyDown={keyDown}
        onClick={click}
      >
        <span ref={trackRef} className="squish-switch__track">
          <motion.span className="squish-switch__thumb" aria-hidden="true" style={{ x }} />
        </span>
      </button>
      {label ? <label htmlFor={buttonId} className="squish-switch__label">{label}</label> : null}
    </span>
  );
};

export default SquishSwitch;
