'use client';

import React, { useCallback, useEffect, useMemo, useRef, useState, type CSSProperties } from 'react';
import { animate, useMotionValue, useMotionValueEvent, useReducedMotion } from 'motion/react';
import './WakeSlider.css';

export interface WakeSliderProps {
  value?: number;
  defaultValue?: number;
  onChange?: (value: number) => void;
  min?: number;
  max?: number;
  step?: number;
  bars?: number;
  height?: number;
  restHeight?: number;
  gap?: number;
  fillColor?: string;
  trackColor?: string;
  duration?: number; // milliseconds; 0 disables animation
  wheel?: boolean;
  wheelStep?: number; // one conventional wheel notch
  showValue?: boolean;
  formatValue?: (value: number) => string;
  disabled?: boolean;
  ariaLabel?: string;
  className?: string;
}

const EASE_OUT: [number, number, number, number] = [0.23, 1, 0.32, 1];
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
const toPct = (v: number, min: number, max: number) => max > min ? ((v - min) / (max - min)) * 100 : 0;
const snap = (v: number, min: number, max: number, step: number) => {
  if (!(max > min)) return min;
  if (!(step > 0)) return clamp(v, min, max);
  const lastWhole = min + Math.floor(+((max - min) / step).toFixed(6)) * step;
  const grid = clamp(Math.round((v - min) / step) * step + min, min, lastWhole);
  return +(lastWhole < max && Math.abs(v - max) <= Math.abs(v - grid) ? max : grid).toFixed(6);
};

const WakeSlider: React.FC<WakeSliderProps> = ({
  value: valueProp,
  defaultValue = 50,
  onChange,
  min = 0,
  max = 100,
  step = 1,
  bars = 32,
  height = 24,
  restHeight = 12,
  gap = 4,
  fillColor = '#4C5ED6',
  trackColor = '#D9DEEA',
  duration = 220,
  wheel = true,
  wheelStep,
  showValue = true,
  formatValue,
  disabled = false,
  ariaLabel = 'Value',
  className = ''
}) => {
  const [inner, setInner] = useState(defaultValue);
  const value = clamp(valueProp ?? inner, min, max);
  const pct = toPct(value, min, max);
  const reduce = useReducedMotion();
  const format = formatValue ?? String;
  const barCount = Math.max(1, Math.floor(bars));
  const rest = clamp(restHeight / Math.max(1, height), 0, 1);

  const rootRef = useRef<HTMLDivElement>(null);
  const trackRef = useRef<HTMLDivElement>(null);
  const handleRef = useRef<HTMLButtonElement>(null);
  const barEls = useRef<(HTMLSpanElement | null)[]>([]);
  const pointerId = useRef<number | null>(null);
  const latest = useRef(value);
  latest.current = value;
  const commitRef = useRef<(next: number) => void>(() => {});

  // Only one animated position; no useSpring, velocity, or wave geometry.
  const displayedPct = useMotionValue(pct);
  const draw = useCallback((percent: number) => {
    const lit = Math.round(clamp(percent, 0, 100) / 100 * (barCount - 1));
    for (let i = 0; i < barCount; i++) {
      const el = barEls.current[i];
      if (!el) continue;
      const on = i <= lit ? 'true' : 'false';
      if (el.dataset.on !== on) el.dataset.on = on;
    }
  }, [barCount]);

  useMotionValueEvent(displayedPct, 'change', draw);
  useEffect(() => {
    draw(displayedPct.get());
  }, [barCount, draw, displayedPct]);

  useEffect(() => {
    if (reduce || duration <= 0) {
      displayedPct.jump(pct);
      draw(pct);
      return;
    }
    // Calling animate on the same MotionValue interrupts the previous tween
    // and starts from the currently displayed value.
    const control = animate(displayedPct, pct, {
      type: 'tween',
      duration: duration / 1000,
      ease: EASE_OUT
    });
    return () => control.stop();
  }, [pct, reduce, duration, displayedPct, draw]);

  const commit = (next: number) => {
    const clean = snap(next, min, max, step);
    if (clean === latest.current) return;
    latest.current = clean;
    if (valueProp === undefined) setInner(clean);
    onChange?.(clean);
  };

  commitRef.current = commit;

  // Non-passive listener is required to prevent page scrolling while adjusting.
  // A wheel notch changes one configured unit; tiny touchpad deltas accumulate.
  useEffect(() => {
    const el = trackRef.current;
    if (!el || !wheel || disabled) return;
    let accumulated = 0;
    const onWheel = (event: WheelEvent) => {
      if (event.ctrlKey) return; // Respect pinch-to-zoom gestures.
      const raw = event.deltaY !== 0 ? event.deltaY : event.deltaX;
      if (!raw) return;
      const units = event.deltaMode === 1 ? raw / 3 : event.deltaMode === 2 ? raw * 3 : raw / 100;
      accumulated += units;
      // Clamp unusually large device deltas; preserve sub-notch precision.
      const notches = Math.trunc(accumulated);
      if (notches !== 0) {
        accumulated -= notches;
        const size = wheelStep && wheelStep > 0 ? wheelStep : step > 0 ? step : (max - min) / 100;
        commitRef.current(latest.current - Math.max(-5, Math.min(5, notches)) * size);
      }
      event.preventDefault();
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [wheel, disabled, wheelStep, min, max, step]);

  const commitFromX = (clientX: number) => {
    const track = trackRef.current;
    if (!track) return;
    const rect = track.getBoundingClientRect();
    if (!rect.width) return;
    let ratio = clamp((clientX - rect.left) / rect.width, 0, 1);
    if (getComputedStyle(track).direction === 'rtl') ratio = 1 - ratio;
    commit(min + ratio * (max - min));
  };

  const onPointerDown = (e: React.PointerEvent<HTMLDivElement>) => {
    if (disabled || pointerId.current !== null || e.button !== 0) return;
    pointerId.current = e.pointerId;
    e.currentTarget.setPointerCapture(e.pointerId);
    handleRef.current?.focus({ preventScroll: true });
    commitFromX(e.clientX);
  };
  const onPointerMove = (e: React.PointerEvent<HTMLDivElement>) => {
    if (e.pointerId === pointerId.current) commitFromX(e.clientX);
  };
  const endDrag = (e: React.PointerEvent<HTMLDivElement>) => {
    if (e.pointerId !== pointerId.current) return;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
    pointerId.current = null;
  };
  const onKeyDown = (e: React.KeyboardEvent<HTMLButtonElement>) => {
    if (disabled) return;
    const jumps: Record<string, number> = {
      ArrowRight: value + step,
      ArrowUp: value + step,
      ArrowLeft: value - step,
      ArrowDown: value - step,
      PageUp: value + step * 10,
      PageDown: value - step * 10,
      Home: min,
      End: max
    };
    if (!(e.key in jumps)) return;
    e.preventDefault();
    commit(jumps[e.key]);
  };

  const barNodes = useMemo(() => Array.from({ length: barCount }, (_, i) => (
    <span key={i} ref={el => { barEls.current[i] = el; }} className="wake-slider__bar" />
  )), [barCount]);

  return (
    <div
      ref={rootRef}
      className={`wake-slider${className ? ` ${className}` : ''}`}
      aria-disabled={disabled || undefined}
      style={{
        '--ws-fill': fillColor,
        '--ws-track': trackColor,
        '--ws-height': `${height}px`,
        '--ws-gap': `${gap}px`,
        '--ws-rest': rest
      } as CSSProperties}
    >
      <div
        ref={trackRef}
        className="wake-slider__track"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onLostPointerCapture={endDrag}
      >
        {barNodes}
        <button
          ref={handleRef}
          type="button"
          role="slider"
          className="wake-slider__handle"
          tabIndex={disabled ? -1 : 0}
          aria-label={ariaLabel}
          aria-valuemin={min}
          aria-valuemax={max}
          aria-valuenow={value}
          aria-valuetext={format(value)}
          aria-disabled={disabled || undefined}
          onKeyDown={onKeyDown}
        />
      </div>
      {showValue ? <span className="wake-slider__value" aria-hidden="true">{format(value)}</span> : null}
    </div>
  );
};

export default WakeSlider;
