import type { TrackedMovement, WorkoutLogEntry } from '../types'

export const TRACKED_MOVEMENT_LABELS: Record<TrackedMovement, string> = {
  pullUps: 'Подтягивания',
  pushUps: 'Отжимания',
  dips: 'Отжимания на брусьях',
  squats: 'Приседания',
  plank: 'Планка',
  other: 'Другое',
}

export interface MovementPoint {
  date: string
  value: number
}

/** Chronological list of logged results for one tracked movement. */
export function getMovementHistory(
  log: WorkoutLogEntry[],
  movement: TrackedMovement,
): MovementPoint[] {
  const points: MovementPoint[] = []
  for (const entry of [...log].sort((a, b) => a.date.localeCompare(b.date))) {
    for (const ex of entry.exercises) {
      if (ex.tracks === movement && ex.actual != null) {
        points.push({ date: entry.date, value: ex.actual })
      }
    }
  }
  return points
}

function daysAgo(days: number): string {
  const d = new Date()
  d.setDate(d.getDate() - days)
  return d.toISOString()
}

export interface MovementSummary {
  movement: TrackedMovement
  before: number
  after: number
}

/**
 * Compares the best result from ~14 days ago against the most recent one,
 * for every movement that has at least one data point in that window.
 */
export function getTwoWeekSummary(log: WorkoutLogEntry[]): MovementSummary[] {
  const cutoff = daysAgo(14)
  const movements: TrackedMovement[] = ['pullUps', 'pushUps', 'dips', 'squats', 'plank']
  const summaries: MovementSummary[] = []

  for (const movement of movements) {
    const history = getMovementHistory(log, movement)
    if (history.length === 0) continue

    const withinWindow = history.filter((p) => p.date >= cutoff)
    const beforeWindow = history.filter((p) => p.date < cutoff)

    const after = history[history.length - 1].value
    const before =
      beforeWindow.length > 0
        ? beforeWindow[beforeWindow.length - 1].value
        : withinWindow[0]?.value ?? after

    summaries.push({ movement, before, after })
  }

  return summaries
}

/** True once the user's last 3 sessions all had "low" energy. */
export function hasLowEnergyStreak(log: WorkoutLogEntry[]): boolean {
  if (log.length < 3) return false
  const lastThree = [...log]
    .sort((a, b) => a.date.localeCompare(b.date))
    .slice(-3)
  return lastThree.every((entry) => entry.energyLevel === 'low')
}

export function formatDelta(before: number, after: number): string {
  const diff = after - before
  if (diff === 0) return `было и есть ${after}`
  const sign = diff > 0 ? '+' : ''
  return `${before} → ${after} (${sign}${diff})`
}
