import { useEffect, useRef, useState } from 'react';

type Point = { x: number; y: number };

interface Stroke {
	cursor: 'maya' | 'june';
	color: string;
	width: number;
	duration: number;
	points: Point[];
}

interface UserStroke {
	color: string;
	width: number;
	erase: boolean;
	points: Point[];
}

interface Props {
	src: string;
	alt: string;
	width: number;
	height: number;
}

const CURSOR_STYLE: Record<Stroke['cursor'], { color: string; label: string }> = {
	maya: { color: '#9dccca', label: 'maya' },
	june: { color: '#ff66c4', label: 'june' },
};

const PEN_COLORS = ['#9dccca', '#ff66c4', '#ffde59', '#7ed957', '#8c52ff', '#ffffff'];

function ellipse(cx: number, cy: number, rx: number, ry: number, wobble = 0.0035): Point[] {
	const points: Point[] = [];
	const steps = 92;
	for (let i = 0; i <= steps; i++) {
		const t = (i / steps) * Math.PI * 2.12;
		const w = 1 + Math.sin(t * 3.5) * wobble * 18;
		points.push({ x: cx + Math.cos(t) * rx * w, y: cy + Math.sin(t) * ry * w });
	}
	return points;
}



const STROKES: Stroke[] = [
	{
		cursor: 'maya',
		color: '#9dccca',
		width: 3.4,
		duration: 1050,
		points: ellipse(0.5, 0.081, 0.205, 0.075),
	},
	{
		cursor: 'june',
		color: '#ff66c4',
		width: 3,
		duration: 820,
		points: [
			{ x: 0.408, y: 0.602 },
			{ x: 0.382, y: 0.62 },
			{ x: 0.356, y: 0.64 },
			{ x: 0.33, y: 0.66 },
			{ x: 0.312, y: 0.672 },
			{ x: 0.334, y: 0.652 },
			{ x: 0.312, y: 0.672 },
			{ x: 0.336, y: 0.688 },
		],
	},
];

function CursorGlyph({ color }: { color: string }) {
	return (
		<svg viewBox="0 0 17 22" aria-hidden="true">
			<path
				d="M2 1l12 10-6.2 1.2L10.6 19 7.5 20 4.6 12.6 1 15z"
				fill={color}
				stroke="#16181d"
				strokeWidth="1.4"
				strokeLinejoin="round"
			/>
		</svg>
	);
}

function PenIcon() {
	return (
		<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4" aria-hidden="true">
			<path d="M2 12l1-4 7-7 3 3-7 7-4 1z" />
			<path d="M8.5 2.5l3 3" />
		</svg>
	);
}

function EraserIcon() {
	return (
		<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4" aria-hidden="true">
			<path d="M3 10l7-7 2 2-7 7H3z" />
			<path d="M2 12h10" />
		</svg>
	);
}

function UndoIcon() {
	return (
		<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4" aria-hidden="true">
			<path d="M3 7a4 4 0 104-4" />
			<path d="M7 0.8V3L5 1.9z" fill="currentColor" stroke="none" />
		</svg>
	);
}

function ReplayIcon() {
	return (
		<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4" aria-hidden="true">
			<path d="M11.4 5A4.5 4.5 0 102.9 9" />
			<path d="M11.8 1.7v3.5H8.3" />
		</svg>
	);
}

function CheckIcon() {
	return (
		<svg viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" aria-hidden="true">
			<path d="M2.4 7.2l3 3 6.2-6.6" />
		</svg>
	);
}

export default function CollaborationStage({ src, alt, width, height }: Props) {
	const wrapRef = useRef<HTMLDivElement>(null);
	const canvasRef = useRef<HTMLCanvasElement>(null);
	const cursorRefs = useRef<Record<string, HTMLDivElement | null>>({});
	const completedRef = useRef<Stroke[]>([]);
	const userRef = useRef<UserStroke[]>([]);
	const sizeRef = useRef({ w: 0, h: 0, dpr: 1 });
	const runningRef = useRef(false);
	const cancelRef = useRef(false);
	const startedRef = useRef(false);
	const pointerRef = useRef<{ active: boolean; last: Point | null }>({ active: false, last: null });

	const [drawing, setDrawing] = useState(false);
	const [color, setColor] = useState(PEN_COLORS[1]);
	const [eraser, setEraser] = useState(false);

	const colorsRef = useRef({ color, eraser });
	colorsRef.current = { color, eraser };

	function setupCanvas() {
		const canvas = canvasRef.current;
		if (!canvas) return;
		const rect = canvas.getBoundingClientRect();
		const dpr = Math.min(window.devicePixelRatio || 1, 2.5);
		const w = Math.max(1, rect.width);
		const h = Math.max(1, rect.height);
		sizeRef.current = { w, h, dpr };
		canvas.width = Math.round(w * dpr);
		canvas.height = Math.round(h * dpr);
		const ctx = canvas.getContext('2d');
		if (!ctx) return;
		ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
		ctx.lineCap = 'round';
		ctx.lineJoin = 'round';
	}

	function drawSegment(a: Point, b: Point, stroke: Stroke | UserStroke) {
		const ctx = canvasRef.current?.getContext('2d');
		if (!ctx) return;
		const { w, h } = sizeRef.current;
		const scale = Math.max(w, h) / 1440;
		ctx.save();
		ctx.globalCompositeOperation = 'erase' in stroke && stroke.erase ? 'destination-out' : 'source-over';
		ctx.strokeStyle = stroke.color;
		ctx.lineWidth = Math.max(1.4, stroke.width * Math.max(0.55, scale));
		ctx.beginPath();
		ctx.moveTo(a.x * w, a.y * h);
		ctx.lineTo(b.x * w, b.y * h);
		ctx.stroke();
		if (a.x === b.x && a.y === b.y) {
			ctx.fillStyle = stroke.color;
			ctx.beginPath();
			ctx.arc(a.x * w, a.y * h, ctx.lineWidth / 2, 0, Math.PI * 2);
			ctx.fill();
		}
		ctx.restore();
	}

	function drawWholeStroke(stroke: Stroke | UserStroke) {
		const ctx = canvasRef.current?.getContext('2d');
		if (!ctx || stroke.points.length < 2) return;
		const { w, h } = sizeRef.current;
		const scale = Math.max(w, h) / 1440;
		ctx.save();
		ctx.globalCompositeOperation = 'erase' in stroke && stroke.erase ? 'destination-out' : 'source-over';
		ctx.strokeStyle = stroke.color;
		ctx.lineWidth = Math.max(1.4, stroke.width * Math.max(0.55, scale));
		ctx.beginPath();
		stroke.points.forEach((p, i) => {
			if (i === 0) ctx.moveTo(p.x * w, p.y * h);
			else ctx.lineTo(p.x * w, p.y * h);
		});
		ctx.stroke();
		ctx.restore();
	}

	function redraw() {
		setupCanvas();
		const ctx = canvasRef.current?.getContext('2d');
		if (!ctx) return;
		const { w, h } = sizeRef.current;
		ctx.clearRect(0, 0, w, h);
		for (const stroke of completedRef.current) drawWholeStroke(stroke);
		for (const stroke of userRef.current) drawWholeStroke(stroke);
	}

	function moveCursor(id: string, p: Point, visible: boolean) {
		const el = cursorRefs.current[id];
		if (!el) return;
		const { w, h } = sizeRef.current;
		el.style.opacity = visible ? '1' : '0';
		el.style.transform = `translate(${p.x * w}px, ${p.y * h}px)`;
	}

	const wait = (ms: number) =>
		new Promise<void>((resolve) => {
			window.setTimeout(resolve, ms);
		});

	async function playStroke(stroke: Stroke) {
		const { w, h } = sizeRef.current;
		if (!w || !h) return;
		const el = cursorRefs.current[stroke.cursor];
		if (el) el.style.opacity = '1';
		const start = stroke.points[0];
		moveCursor(stroke.cursor, start, true);
		await wait(160);
		if (cancelRef.current) return;

		const pts = stroke.points;
		const total = pts.length;
		let index = 0;
		const t0 = performance.now();

		await new Promise<void>((resolve) => {
			const tick = (now: number) => {
				if (cancelRef.current) return resolve();
				const p = Math.min(1, (now - t0) / stroke.duration);
				const eased = p < 0.5 ? 2 * p * p : 1 - Math.pow(-2 * p + 2, 2) / 2;
				const target = Math.min(total - 1, Math.floor(eased * (total - 1)));
				while (index < target) {
					drawSegment(pts[index], pts[index + 1], stroke);
					index++;
				}
				moveCursor(stroke.cursor, pts[target], true);
				if (p < 1) requestAnimationFrame(tick);
				else resolve();
			};
			requestAnimationFrame(tick);
		});

		if (cancelRef.current) return;
		completedRef.current.push(stroke);
		await wait(520);
		if (cancelRef.current) return;
		moveCursor(stroke.cursor, pts[pts.length - 1], false);
		await wait(90);
	}

	async function playIntro() {
		if (runningRef.current) return;
		runningRef.current = true;
		cancelRef.current = false;
		startedRef.current = true;
		redraw();
		await wait(150);
		for (const stroke of STROKES) {
			if (cancelRef.current) break;
			await playStroke(stroke);
		}
		runningRef.current = false;
	}

	function finishInstantly() {
		completedRef.current = STROKES.slice();
		redraw();
		runningRef.current = false;
		startedRef.current = true;
	}

	useEffect(() => {
		const canvas = canvasRef.current;
		const wrap = wrapRef.current;
		if (!canvas || !wrap) return;

		setupCanvas();
		const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

		let observer: IntersectionObserver | null = null;
		if (reduce) {
			finishInstantly();
		} else if ('IntersectionObserver' in window) {
			observer = new IntersectionObserver(
				(entries) => {
					if (entries.some((e) => e.isIntersecting) && !startedRef.current) {
						playIntro();
					}
				},
				{ threshold: 0.25 }
			);
			observer.observe(wrap);
		} else {
			playIntro();
		}

		const ro = new ResizeObserver(() => {
			if (!startedRef.current) {
				setupCanvas();
				return;
			}
			if (runningRef.current) {
				cancelRef.current = true;
				runningRef.current = false;
			}
			redraw();
		});
		ro.observe(wrap);

		return () => {
			cancelRef.current = true;
			runningRef.current = false;
			observer?.disconnect();
			ro.disconnect();
		};
	}, []);

	useEffect(() => {
		if (!drawing) {
			pointerRef.current.active = false;
			return;
		}
		const onKey = (e: KeyboardEvent) => {
			if (e.key === 'Escape') setDrawing(false);
		};
		window.addEventListener('keydown', onKey);
		return () => window.removeEventListener('keydown', onKey);
	}, [drawing]);

	function pointFromEvent(e: React.PointerEvent<HTMLCanvasElement>): Point {
		const canvas = canvasRef.current;
		if (!canvas) return { x: 0, y: 0 };
		const rect = canvas.getBoundingClientRect();
		return {
			x: Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width)),
			y: Math.min(1, Math.max(0, (e.clientY - rect.top) / rect.height)),
		};
	}

	function onPointerDown(e: React.PointerEvent<HTMLCanvasElement>) {
		if (!drawing) return;
		e.preventDefault();
		const canvas = canvasRef.current;
		if (!canvas) return;
		canvas.setPointerCapture(e.pointerId);
		const p = pointFromEvent(e);
		const stroke: UserStroke = {
			color: colorsRef.current.color,
			width: 3,
			erase: colorsRef.current.eraser,
			points: [p],
		};
		userRef.current.push(stroke);
		pointerRef.current = { active: true, last: p };
		drawSegment(p, p, stroke);
	}

	function onPointerMove(e: React.PointerEvent<HTMLCanvasElement>) {
		if (!drawing || !pointerRef.current.active) return;
		const p = pointFromEvent(e);
		const last = pointerRef.current.last;
		const stroke = userRef.current[userRef.current.length - 1];
		if (!stroke || !last) return;
		stroke.points.push(p);
		drawSegment(last, p, stroke);
		pointerRef.current.last = p;
	}

	function onPointerUp(e: React.PointerEvent<HTMLCanvasElement>) {
		if (!pointerRef.current.active) return;
		pointerRef.current = { active: false, last: null };
		const canvas = canvasRef.current;
		if (canvas && canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
	}

	function undo() {
		userRef.current.pop();
		redraw();
	}

	function clearAll() {
		cancelRef.current = true;
		hideCursors();
		userRef.current = [];
		completedRef.current = [];
		const ctx = canvasRef.current?.getContext('2d');
		if (!ctx) return;
		const { w, h } = sizeRef.current;
		ctx.clearRect(0, 0, w, h);
	}

	function replay() {
		cancelRef.current = true;
		window.setTimeout(() => {
			userRef.current = [];
			completedRef.current = [];
			const ctx = canvasRef.current?.getContext('2d');
			if (ctx) {
				const { w, h } = sizeRef.current;
				ctx.clearRect(0, 0, w, h);
			}
			runningRef.current = false;
			cancelRef.current = false;
			playIntro();
		}, 60);
	}

	function hideCursors() {
		for (const el of Object.values(cursorRefs.current)) {
			if (el) el.style.opacity = '0';
		}
	}

	function toggleDrawing() {
		if (drawing) {
			setDrawing(false);
			return;
		}
		if (runningRef.current) {
			cancelRef.current = true;
			runningRef.current = false;
			hideCursors();
		}
		if (completedRef.current.length === 0) {
			finishInstantly();
		}
		setDrawing(true);
	}

	return (
		<div className="collab-stage" ref={wrapRef} data-drawing={drawing ? 'true' : 'false'}>
			<img
				src={src}
				width={width}
				height={height}
				alt={alt}
				loading="eager"
				decoding="async"
				draggable={false}
			/>
			<div className="ink-layer">
				<canvas
					ref={canvasRef}
					className="ink-canvas"
					aria-label="Drawing surface over the screen share. Activate the pen button to draw."
					onPointerDown={onPointerDown}
					onPointerMove={onPointerMove}
					onPointerUp={onPointerUp}
					onPointerCancel={onPointerUp}
				/>
				{STROKES.map((stroke, i) => {
					const style = CURSOR_STYLE[stroke.cursor];
					return (
						<div
							key={`${stroke.cursor}-${i}`}
							className="stage-cursor"
							style={{ ['--cursor-color' as string]: style.color }}
							ref={(el) => {
								cursorRefs.current[stroke.cursor] = el;
							}}
							aria-hidden="true"
						>
							<CursorGlyph color={style.color} />
							<span className="tag">{style.label}</span>
						</div>
					);
				})}
			</div>

			<div
				className="stage-tools"
				role="toolbar"
				aria-label={drawing ? 'Drawing tools' : 'Try the collaborative drawing'}
			>
				{!drawing ? (
					<>
						<button type="button" className="tool-btn pen-cta" onClick={toggleDrawing}>
							<PenIcon /> Try the pen
						</button>
						<span className="hint">everyone gets a pen</span>
					</>
				) : (
					<>
						<div className="swatches">
							{PEN_COLORS.map((c) => (
								<button
									key={c}
									type="button"
									className="swatch"
									style={{ background: c }}
									aria-label={`Pen color ${c}`}
									aria-pressed={!eraser && color === c}
									onClick={() => {
										setColor(c);
										setEraser(false);
									}}
								/>
							))}
						</div>
						<span className="vr" />
						<button
							type="button"
							className="tool-btn"
							aria-pressed={eraser}
							aria-label="Eraser"
							onClick={() => setEraser((v) => !v)}
						>
							<EraserIcon /> <span className="label">Eraser</span>
						</button>
						<button type="button" className="tool-btn" aria-label="Undo" onClick={undo}>
							<UndoIcon /> <span className="label">Undo</span>
						</button>
						<button
							type="button"
							className="tool-btn subtle"
							aria-label="Clear all ink"
							onClick={clearAll}
						>
							Clear
						</button>
						<span className="vr" />
						<button type="button" className="tool-btn pen-cta" onClick={() => setDrawing(false)}>
							<CheckIcon /> <span className="label">Done</span>
						</button>
					</>
				)}
				<button
					type="button"
					className="tool-btn replay"
					aria-label="Replay the cursor intro"
					title="Replay the intro"
					onClick={replay}
				>
					<ReplayIcon />
				</button>
			</div>
			{drawing && (
				<p
					style={{
						position: 'absolute',
						left: '50%',
						bottom: '76px',
						transform: 'translateX(-50%)',
						zIndex: 6,
						margin: 0,
						fontSize: '10.5px',
						color: '#c6d0d0',
						background: 'rgba(18,21,26,.78)',
						border: '1px solid rgba(95,105,116,.45)',
						borderRadius: '6px',
						padding: '4px 8px',
						whiteSpace: 'nowrap',
						pointerEvents: 'none',
					}}
				>
					Draw on the share — nothing is saved.
				</p>
			)}
		</div>
	);
}
