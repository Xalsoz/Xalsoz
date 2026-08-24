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
  high: 3,
}

const ENERGY_INTENSITY_MULT: Record<EnergyLevel, number> = {
  low: 0.6,
  medium: 0.85,
  high: 1.05,
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
    const target = Math.max(3, Math.round(base * mult))
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

export function generateProgram(params: GenerateParams): GenerateResult {
  const { environment, energyLevel, profile, workoutLog, programHistory, progressionState } =
    params

  const pool = getExercisesForEnvironment(environment)
  const [minDiff, maxDiff] = ENERGY_DIFFICULTY_RANGE[energyLevel]
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
      if (energyLevel === 'low' || selected.length >= 5 || Math.random() < 0.4) {
        continue
      }
    }

    let candidates = pool.filter(
      (ex) =>
        ex.muscleGroup === slot.muscleGroup &&
        ex.difficulty >= minDiff &&
        ex.difficulty <= maxDiff &&
        !usedIds.has(ex.id) &&
        !(energyLevel === 'low' && EXCLUDE_ON_LOW_ENERGY.has(ex.id)),
    )

    if (candidates.length === 0) {
      candidates = pool.filter(
        (ex) => ex.muscleGroup === slot.muscleGroup && !usedIds.has(ex.id),
      )
    }
    if (candidates.length === 0) continue

    // Only offer exercises the user has "unlocked" in their progression chain.
    const unlocked = candidates.filter((ex) => {
      if (!ex.progressionGroup || ex.progressionLevel == null) return true
      const level = getCurrentLevel(ex.progressionGroup, progressionState, profile)
      return ex.progressionLevel <= level
    })
    const pickPool = unlocked.length > 0 ? unlocked : candidates

    // Avoid repeating exactly last session's set for this environment+energy combo.
    let rotationCandidates = pickPool.filter((ex) => !lastExerciseIds.has(ex.id))
    if (rotationCandidates.length === 0) rotationCandidates = pickPool

    // Prefer exercises not seen in the last 3 generated programs at all, for novelty.
    const fresh = rotationCandidates.filter((ex) => !recentAllIds.has(ex.id))
    const finalPool = fresh.length > 0 ? fresh : rotationCandidates

    let chosen: ExerciseDef
    const chainCandidates = finalPool.filter((ex) => ex.progressionGroup)
    if (chainCandidates.length > 0) {
      // Within the chain, stay at the highest unlocked level (the "current" progression step).
      const maxLevel = Math.max(...chainCandidates.map((e) => e.progressionLevel ?? 0))
      const atLevel = chainCandidates.filter((e) => e.progressionLevel === maxLevel)
      chosen = atLevel[Math.floor(Math.random() * atLevel.length)]
    } else {
      chosen = finalPool[Math.floor(Math.random() * finalPool.length)]
    }

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
