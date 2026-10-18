'use client';

import React, {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type CSSProperties,
  type ReactNode
} from 'react';
import { animate, motion, useMotionValue, useReducedMotion, useTransform } from 'motion/react';
import './RubberSegment.css';

export type RubberSegmentSize = 'sm' | 'md' | 'lg';
export type RubberSegmentItem = string | { value: string; label: ReactNode; icon?: ReactNode };

export interface RubberSegmentProps {
  items: RubberSegmentItem[];
  value?: string;
  defaultValue?: string;
  onChange?: (value: string, index: number) => void;
  trackColor?: string;
  thumbColor?: string;
  textColor?: string;
  activeTextColor?: string;
  size?: RubberSegmentSize;
  radius?: number;
  inset?: number;
  equalSlots?: boolean;
  duration?: number; // milliseconds, default 220
  disabled?: boolean;
  className?: string;
  'aria-label'?: string;
}

type Slot = { left: number; right: number };
const EASE_OUT: [number, number, number, number] = [0.23, 1, 0.32, 1];
const SIZES: Record<RubberSegmentSize, { height: number; font: number; pad: number; min: number }> = {
  sm: { height: 28, font: 12, pad: 10, min: 36 },
  md: { height: 36, font: 13, pad: 14, min: 44 },
  lg: { height: 58, font: 16, pad: 18, min: 112 }
};

const RubberSegment: React.FC<RubberSegmentProps> = ({
  items,
  value,
  defaultValue,
  onChange,
  trackColor = '#edeff5',
  thumbColor = '#ffffff',
  textColor = '#626b7e',
  activeTextColor = '#4C5ED6',
  size = 'md',
  radius = 10,
  inset = 3,
  equalSlots = true,
  duration = 220,
  disabled = false,
  className = '',
  'aria-label': ariaLabel = 'Segmented control'
}) => {
  const list = items.map(item => (typeof item === 'string' ? { value: item, label: item } : item));
  const [inner, setInner] = useState<string | undefined>(defaultValue ?? list[0]?.value);
  const current = value !== undefined ? value : inner;
  const index = Math.max(0, list.findIndex(item => item.value === current));
  const reducedMotion = useReducedMotion();

  const trackRef = useRef<HTMLDivElement>(null);
  const itemRefs = useRef<(HTMLButtonElement | null)[]>([]);
  const slotsRef = useRef<Slot[]>([]);
  const measuredRef = useRef(false);

  // Two edges move together using identical tween settings: no stretch/squash/bounce.
  const edgeL = useMotionValue(0);
  const edgeR = useMotionValue(0);
  const innerWidth = useMotionValue(0);
  const thumbRadius = Math.max(0, radius - inset);
  const clipPath = useTransform(
    () => `inset(0 ${Math.max(0, innerWidth.get() - edgeR.get())}px 0 ${Math.max(0, edgeL.get())}px round ${thumbRadius}px)`
  );

  const valuesKey = list.map(item => item.value).join('|');

  // Recalculate layout on mount, resize, or slot geometry changes.
  useLayoutEffect(() => {
    let alive = true;
    const measure = () => {
      if (!alive || !trackRef.current) return;
      const trackRect = trackRef.current.getBoundingClientRect();
      slotsRef.current = list.map((_, i) => {
        const rect = itemRefs.current[i]?.getBoundingClientRect();
        return rect
          ? { left: rect.left - trackRect.left - inset, right: rect.right - trackRect.left - inset }
          : { left: 0, right: 0 };
      });
      innerWidth.set(Math.max(0, trackRect.width - inset * 2));
      const active = slotsRef.current[index];
      if (active) {
        edgeL.stop();
        edgeR.stop();
        edgeL.set(active.left);
        edgeR.set(active.right);
      }
      measuredRef.current = true;
    };

    measure();
    const observer = new ResizeObserver(measure);
    if (trackRef.current) observer.observe(trackRef.current);
    if (document.fonts) void document.fonts.ready.then(measure);
    return () => { alive = false; observer.disconnect(); };
    // index changes are animated in the next effect, not treated as layout changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [valuesKey, size, inset, equalSlots]);

  useEffect(() => {
    if (!measuredRef.current) return;
    const active = slotsRef.current[index];
    if (!active) return;
    // stop() retains the currently visible value so rapid clicks do not jump.
    edgeL.stop();
    edgeR.stop();
    if (reducedMotion || duration <= 0) {
      edgeL.set(active.left);
      edgeR.set(active.right);
      return;
    }
    const config = { type: 'tween' as const, duration: duration / 1000, ease: EASE_OUT };
    const a = animate(edgeL, active.left, config);
    const b = animate(edgeR, active.right, config);
    return () => { a.stop(); b.stop(); };
  }, [index, duration, reducedMotion, edgeL, edgeR, valuesKey]);

  const choose = (next: number) => {
    if (disabled || next < 0 || next >= list.length || next === index) return;
    if (value === undefined) setInner(list[next].value);
    onChange?.(list[next].value, next);
  };

  const onKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>) => {
    if (disabled || list.length === 0) return;
    let next = index;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') next = Math.min(index + 1, list.length - 1);
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') next = Math.max(index - 1, 0);
    else if (e.key === 'Home') next = 0;
    else if (e.key === 'End') next = list.length - 1;
    else return;
    e.preventDefault();
    choose(next);
    itemRefs.current[next]?.focus();
  };

  const preset = SIZES[size] || SIZES.md;
  const style = {
    '--rs-track': trackColor,
    '--rs-thumb': thumbColor,
    '--rs-ink': textColor,
    '--rs-ink-active': activeTextColor,
    '--rs-radius': `${radius}px`,
    '--rs-inset': `${inset}px`,
    '--rs-thumb-radius': `${thumbRadius}px`,
    '--rs-h': `${preset.height}px`,
    '--rs-font': `${preset.font}px`,
    '--rs-pad': `${preset.pad}px`,
    '--rs-min': `${preset.min}px`
  } as CSSProperties;

  return (
    <div
      ref={trackRef}
      role="radiogroup"
      aria-label={ariaLabel}
      aria-disabled={disabled || undefined}
      data-equal={equalSlots ? '' : undefined}
      className={`rubber-segment${className ? ` ${className}` : ''}`}
      style={style}
    >
      {list.map((item, i) => (
        <button
          key={item.value}
          ref={element => { itemRefs.current[i] = element; }}
          type="button"
          role="radio"
          aria-checked={i === index}
          tabIndex={i === index ? 0 : -1}
          disabled={disabled}
          className="rubber-segment__item"
          onClick={() => choose(i)}
          onKeyDown={onKeyDown}
        >
          {item.icon}
          {item.label}
        </button>
      ))}
      <motion.div className="rubber-segment__thumb" aria-hidden="true" style={{ clipPath }}>
        {list.map(item => (
          <span key={item.value} className="rubber-segment__item rubber-segment__copy">
            {item.icon}
            {item.label}
          </span>
        ))}
      </motion.div>
    </div>
  );
};

export default RubberSegment;
