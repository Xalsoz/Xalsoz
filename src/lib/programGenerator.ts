import { EXERCISE_POOL, getExercisesForEnvironment } from './exercisePool'
import type {
  EnergyLevel,
  Environment,
  ExerciseDef,
  GeneratedProgramRecord,
  ProgressionState,
  TrackedMovement,
  UserProfile,
  WorkoutLogEntry,
} from '../types'

export interface GeneratedExercise {
  exerciseId: string
  name: string
  muscleGroup: ExerciseDef['muscleGroup']
  unit: ExerciseDef['unit']
  sets: number
  target: number
  variant?: string
  description: string
  tracks: TrackedMovement
}

export interface GeneratedProgram {
  comboKey: string
  exercises: GeneratedExercise[]
}

const ENERGY_DIFFICULTY_RANGE: Record<EnergyLevel, [number, number]> = {
  low: [1, 3],
  medium: [1, 4],
  high: [2, 5],
}

const ENERGY_SET_COUNT: Record<EnergyLevel, number> = {
  low: 2,
  medium: 3,
  high: 4,
}

const ENERGY_INTENSITY_MULT: Record<EnergyLevel, number> = {
  low: 0.6,
  medium: 0.85,
  high: 1.05,
}

/**
 * A harder variant of a tracked movement (e.g. pike/diamond/archer push-ups)
 * realistically yields far fewer reps than the user's flat push-up/pull-up
 * max, and an easier one (wall push-ups, dead hangs) yields more. Scale the
 * rep target by how far the exercise's difficulty sits from the "standard"
 * difficulty (3) for that chain, instead of reusing the raw max as-is.
 */
const DIFFICULTY_REP_FACTOR: Record<number, number> = {
  1: 1.6,
  2: 1.3,
  3: 1.0,
  4: 0.55,
  5: 0.3,
}

const EXCLUDE_ON_LOW_ENERGY = new Set([
  'burpees',
  'jump-squats',
  'clap-pushups',
])

/** Bump a progression chain to its next level after this many sessions on it. */
const PROGRESSION_BUMP_EVERY = 3

const BASELINE_TARGET: Record<
  number,
  { reps: [number, number]; seconds: [number, number] }
> = {
  1: { reps: [6, 10], seconds: [15, 25] },
  2: { reps: [8, 12], seconds: [20, 35] },
  3: { reps: [8, 14], seconds: [25, 45] },
  4: { reps: [6, 10], seconds: [20, 35] },
  5: { reps: [3, 6], seconds: [10, 20] },
}

export function comboKey(environment: Environment, energy: EnergyLevel): string {
  return `${environment}:${energy}`
}

function inferStartLevel(progressionGroup: string, profile: UserProfile): number {
  if (progressionGroup === 'pull-chain') {
    const n = profile.startingLevel.pullUps
    if (n <= 0) return 1
    if (n < 3) return 4
    if (n < 8) return 5
    return 6
  }
  if (progressionGroup === 'push-chain') {
    const n = profile.startingLevel.pushUps
    if (n <= 0) return 1
    if (n < 5) return 2
    if (n < 15) return 3
    return 4
  }
  // dip-chain, squat-chain, core-chain have no direct onboarding number —
  // start conservative and let the rotation logic advance them over time.
  return 1
}

function getCurrentLevel(
  progressionGroup: string,
  state: ProgressionState,
  profile: UserProfile,
): number {
  return state[progressionGroup]?.level ?? inferStartLevel(progressionGroup, profile)
}

function latestActualFor(
  tracks: TrackedMovement,
  log: WorkoutLogEntry[],
): number | null {
  for (let i = log.length - 1; i >= 0; i--) {
    for (const ex of log[i].exercises) {
      if (ex.tracks === tracks && ex.actual != null) return ex.actual
    }
  }
  return null
}

function computeSetsAndTarget(
  exercise: ExerciseDef,
  energyLevel: EnergyLevel,
  log: WorkoutLogEntry[],
  profile: UserProfile,
): { sets: number; target: number } {
  const sets = ENERGY_SET_COUNT[energyLevel]
  const mult = ENERGY_INTENSITY_MULT[energyLevel]

  if (exercise.tracks === 'pullUps' || exercise.tracks === 'pushUps') {
    const startVal =
      exercise.tracks === 'pullUps'
        ? profile.startingLevel.pullUps
        : profile.startingLevel.pushUps
    const currentMax = latestActualFor(exercise.tracks, log) ?? startVal
    const base = Math.max(currentMax, exercise.difficulty <= 2 ? 5 : 3)
    const target = Math.max(3, Math.round(base * mult * DIFFICULTY_REP_FACTOR[exercise.difficulty]))
    return { sets, target }
  }

  const baseline = BASELINE_TARGET[exercise.difficulty]
  const [lo, hi] = exercise.unit === 'seconds' ? baseline.seconds : baseline.reps
  const scale = (mult - 0.6) / (1.05 - 0.6)
  const mid = lo + (hi - lo) * scale
  const target = Math.round(Math.min(hi, Math.max(lo, mid)))
  return { sets, target }
}

function shuffle<T>(arr: T[]): T[] {
  const copy = [...arr]
  for (let i = copy.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    ;[copy[i], copy[j]] = [copy[j], copy[i]]
  }
  return copy
}

interface GenerateParams {
  environment: Environment
  energyLevel: EnergyLevel
  profile: UserProfile
  workoutLog: WorkoutLogEntry[]
  programHistory: GeneratedProgramRecord[]
  progressionState: ProgressionState
}

interface GenerateResult {
  program: GeneratedProgram
  nextProgressionState: ProgressionState
}

const MUSCLE_GROUP_SLOTS: Array<{
  muscleGroup: ExerciseDef['muscleGroup']
  required: boolean
}> = [
  { muscleGroup: 'pull', required: true },
  { muscleGroup: 'push', required: true },
  { muscleGroup: 'legs', required: true },
  { muscleGroup: 'core', required: true },
  { muscleGroup: 'full-body', required: false },
]

interface PickSlotParams {
  muscleGroup: ExerciseDef['muscleGroup']
  pool: ExerciseDef[]
  energyLevel: EnergyLevel
  excludeIds: Set<string>
  lastExerciseIds: Set<string>
  recentAllIds: Set<string>
  progressionState: ProgressionState
  profile: UserProfile
}

/** Candidate selection shared between generating a full program and swapping one slot. */
function pickExerciseForSlot(params: PickSlotParams): ExerciseDef | null {
  const { muscleGroup, pool, energyLevel, excludeIds, lastExerciseIds, recentAllIds, progressionState, profile } =
    params
  const [minDiff, maxDiff] = ENERGY_DIFFICULTY_RANGE[energyLevel]

  let candidates = pool.filter(
    (ex) =>
      ex.muscleGroup === muscleGroup &&
      ex.difficulty >= minDiff &&
      ex.difficulty <= maxDiff &&
      !excludeIds.has(ex.id) &&
      !(energyLevel === 'low' && EXCLUDE_ON_LOW_ENERGY.has(ex.id)),
  )

  if (candidates.length === 0) {
    candidates = pool.filter((ex) => ex.muscleGroup === muscleGroup && !excludeIds.has(ex.id))
  }
  if (candidates.length === 0) return null

  // Offer exercises around the user's current progression level — one step
  // easier or harder too, not just the exact level — for more variety.
  const unlocked = candidates.filter((ex) => {
    if (!ex.progressionGroup || ex.progressionLevel == null) return true
    const level = getCurrentLevel(ex.progressionGroup, progressionState, profile)
    return ex.progressionLevel >= level - 1 && ex.progressionLevel <= level + 1
  })
  const pickPool = unlocked.length > 0 ? unlocked : candidates

  // Avoid repeating exactly last session's set for this environment+energy combo.
  let rotationCandidates = pickPool.filter((ex) => !lastExerciseIds.has(ex.id))
  if (rotationCandidates.length === 0) rotationCandidates = pickPool

  // Prefer exercises not seen in the last 3 generated programs at all, for novelty.
  const fresh = rotationCandidates.filter((ex) => !recentAllIds.has(ex.id))
  const finalPool = fresh.length > 0 ? fresh : rotationCandidates

  // Pick randomly across the whole unlocked window (not always the hardest
  // unlocked step) so the same level doesn't dominate every session.
  return finalPool[Math.floor(Math.random() * finalPool.length)]
}

export function generateProgram(params: GenerateParams): GenerateResult {
  const { environment, energyLevel, profile, workoutLog, programHistory, progressionState } =
    params

  const pool = getExercisesForEnvironment(environment)
  const key = comboKey(environment, energyLevel)

  const recentRecords = programHistory
    .filter((r) => r.comboKey === key)
    .sort((a, b) => b.generatedAt.localeCompare(a.generatedAt))
  const lastExerciseIds = new Set(recentRecords[0]?.exerciseIds ?? [])
  const recentAllIds = new Set(recentRecords.flatMap((r) => r.exerciseIds))

  const nextProgressionState: ProgressionState = { ...progressionState }
  const selected: GeneratedExercise[] = []
  const usedIds = new Set<string>()

  for (const slot of MUSCLE_GROUP_SLOTS) {
    if (selected.length >= 6) break

    if (slot.muscleGroup === 'full-body') {
      if (energyLevel === 'low' || selected.length >= 5) continue
      const skipChance = energyLevel === 'high' ? 0.15 : 0.5
      if (Math.random() < skipChance) continue
    }

    const chosen = pickExerciseForSlot({
      muscleGroup: slot.muscleGroup,
      pool,
      energyLevel,
      excludeIds: usedIds,
      lastExerciseIds,
      recentAllIds,
      progressionState,
      profile,
    })
    if (!chosen) continue

    usedIds.add(chosen.id)

    let variant: string | undefined
    if (chosen.variants && chosen.variants.length > 0) {
      const timesSeen = recentRecords.filter((r) => r.exerciseIds.includes(chosen.id)).length
      variant = chosen.variants[timesSeen % chosen.variants.length]
    }

    const { sets, target } = computeSetsAndTarget(chosen, energyLevel, workoutLog, profile)

    selected.push({
      exerciseId: chosen.id,
      name: chosen.name,
      muscleGroup: chosen.muscleGroup,
      unit: chosen.unit,
      sets,
      target,
      variant,
      description: chosen.description,
      tracks: chosen.tracks,
    })

    if (chosen.progressionGroup) {
      const group = chosen.progressionGroup
      const current =
        nextProgressionState[group] ??
        ({ level: getCurrentLevel(group, progressionState, profile), sessionsAtLevel: 0 } as const)
      const sessionsAtLevel = current.sessionsAtLevel + 1

      if (sessionsAtLevel >= PROGRESSION_BUMP_EVERY) {
        const hasNextLevel = EXERCISE_POOL.some(
          (ex) => ex.progressionGroup === group && ex.progressionLevel === current.level + 1,
        )
        nextProgressionState[group] = hasNextLevel
          ? { level: current.level + 1, sessionsAtLevel: 0 }
          : { level: current.level, sessionsAtLevel: 0 }
      } else {
        nextProgressionState[group] = { level: current.level, sessionsAtLevel }
      }
    }
  }

  // Safety net: guarantee at least 4 exercises even for a sparse environment pool.
  if (selected.length < 4) {
    for (const ex of pool) {
      if (selected.length >= 4) break
      if (usedIds.has(ex.id)) continue
      usedIds.add(ex.id)
      const { sets, target } = computeSetsAndTarget(ex, energyLevel, workoutLog, profile)
      selected.push({
        exerciseId: ex.id,
        name: ex.name,
        muscleGroup: ex.muscleGroup,
        unit: ex.unit,
        sets,
        target,
        description: ex.description,
        tracks: ex.tracks,
      })
    }
  }

  return {
    program: { comboKey: key, exercises: shuffle(selected) },
    nextProgressionState,
  }
}

interface RegenerateExerciseParams {
  muscleGroup: ExerciseDef['muscleGroup']
  environment: Environment
  energyLevel: EnergyLevel
  profile: UserProfile
  workoutLog: WorkoutLogEntry[]
  programHistory: GeneratedProgramRecord[]
  progressionState: ProgressionState
  /** Exercise ids already in the current program, so the swap can't pick a duplicate. */
  excludeIds: string[]
}

/** Swap out a single slot in an already-generated program for a different exercise. */
export function regenerateExercise(params: RegenerateExerciseParams): GeneratedExercise | null {
  const { muscleGroup, environment, energyLevel, profile, workoutLog, programHistory, progressionState, excludeIds } =
    params

  const pool = getExercisesForEnvironment(environment)
  const key = comboKey(environment, energyLevel)
  const recentRecords = programHistory.filter((r) => r.comboKey === key)
  const recentAllIds = new Set(recentRecords.flatMap((r) => r.exerciseIds))
  const excludeSet = new Set(excludeIds)

  const chosen = pickExerciseForSlot({
    muscleGroup,
    pool,
    energyLevel,
    excludeIds: excludeSet,
    lastExerciseIds: excludeSet,
    recentAllIds,
    progressionState,
    profile,
  })
  if (!chosen) return null

  let variant: string | undefined
  if (chosen.variants && chosen.variants.length > 0) {
    const timesSeen = recentRecords.filter((r) => r.exerciseIds.includes(chosen.id)).length
    variant = chosen.variants[timesSeen % chosen.variants.length]
  }

  const { sets, target } = computeSetsAndTarget(chosen, energyLevel, workoutLog, profile)

  return {
    exerciseId: chosen.id,
    name: chosen.name,
    muscleGroup: chosen.muscleGroup,
    unit: chosen.unit,
    sets,
    target,
    variant,
    description: chosen.description,
    tracks: chosen.tracks,
  }
}
