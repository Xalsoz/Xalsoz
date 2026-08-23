// Core domain types. Keep these framework/storage-agnostic so the same
// shapes can later be served by a real backend instead of localStorage.

export type Environment =
  | 'home-no-bar'
  | 'home-bar'
  | 'park'
  | 'gym'
  | 'no-equipment'

export type EnergyLevel = 'low' | 'medium' | 'high'

export type MuscleGroup = 'push' | 'pull' | 'legs' | 'core' | 'full-body'

export type ExerciseUnit = 'reps' | 'seconds'

/** Canonical trackable movement keys used for progress charts. */
export type TrackedMovement =
  | 'pullUps'
  | 'pushUps'
  | 'dips'
  | 'squats'
  | 'plank'
  | 'other'

export interface ExerciseDef {
  id: string
  name: string
  environments: Environment[]
  muscleGroup: MuscleGroup
  unit: ExerciseUnit
  /** 1 = easiest, 5 = hardest, used to filter by energy level / user level */
  difficulty: 1 | 2 | 3 | 4 | 5
  /** Short technique note, no video required for MVP */
  description: string
  /** Optional progression chain this exercise belongs to (e.g. "pull-up-chain") */
  progressionGroup?: string
  /** Position within the progression chain, ascending = harder */
  progressionLevel?: number
  /** Grip / tempo / stance variants used to add novelty between sessions */
  variants?: string[]
  /** Canonical movement this exercise counts towards in progress tracking */
  tracks: TrackedMovement
}

export interface UserProfile {
  startingLevel: {
    pullUps: number
    pushUps: number
  }
  restrictions: string
  weeklyFrequencyGoal: number
  createdAt: string
}

export interface LoggedExercise {
  exerciseId: string
  name: string
  tracks: TrackedMovement
  unit: ExerciseUnit
  sets: number
  /** target reps per set (or target seconds if unit is 'seconds') */
  target: number
  /** what the user actually reported after the session, null = not filled in */
  actual: number | null
}

export interface WorkoutLogEntry {
  id: string
  date: string
  environment: Environment
  energyLevel: EnergyLevel
  exercises: LoggedExercise[]
}

export interface GeneratedProgramRecord {
  comboKey: string
  exerciseIds: string[]
  generatedAt: string
}

export interface ProgressionState {
  [progressionGroupId: string]: {
    level: number
    sessionsAtLevel: number
  }
}

export interface AppData {
  userProfile: UserProfile | null
  workoutLog: WorkoutLogEntry[]
  programHistory: GeneratedProgramRecord[]
  progressionState: ProgressionState
}
