import type {
  AppData,
  GeneratedProgramRecord,
  ProgressionState,
  UserProfile,
  WorkoutLogEntry,
} from '../types'

/**
 * Repository interface the rest of the app depends on. The localStorage
 * implementation below is a drop-in for MVP; swapping to a real backend
 * later only means writing a new class that implements this interface
 * (e.g. one backed by fetch calls) and changing the export at the bottom.
 */
export interface DataRepository {
  getUserProfile(): Promise<UserProfile | null>
  saveUserProfile(profile: UserProfile): Promise<void>

  getWorkoutLog(): Promise<WorkoutLogEntry[]>
  appendWorkoutLog(entry: WorkoutLogEntry): Promise<void>

  getProgramHistory(): Promise<GeneratedProgramRecord[]>
  appendProgramRecord(record: GeneratedProgramRecord): Promise<void>

  getProgressionState(): Promise<ProgressionState>
  saveProgressionState(state: ProgressionState): Promise<void>

  clearAll(): Promise<void>
}

const KEYS = {
  userProfile: 'xalsoz:userProfile',
  workoutLog: 'xalsoz:workoutLog',
  programHistory: 'xalsoz:programHistory',
  progressionState: 'xalsoz:progressionState',
} as const

const MAX_PROGRAM_RECORDS_PER_COMBO = 3

function readJson<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key)
    if (!raw) return fallback
    return JSON.parse(raw) as T
  } catch {
    return fallback
  }
}

function writeJson<T>(key: string, value: T): void {
  localStorage.setItem(key, JSON.stringify(value))
}

class LocalStorageRepository implements DataRepository {
  async getUserProfile(): Promise<UserProfile | null> {
    return readJson<UserProfile | null>(KEYS.userProfile, null)
  }

  async saveUserProfile(profile: UserProfile): Promise<void> {
    writeJson(KEYS.userProfile, profile)
  }

  async getWorkoutLog(): Promise<WorkoutLogEntry[]> {
    return readJson<WorkoutLogEntry[]>(KEYS.workoutLog, [])
  }

  async appendWorkoutLog(entry: WorkoutLogEntry): Promise<void> {
    const log = await this.getWorkoutLog()
    log.push(entry)
    writeJson(KEYS.workoutLog, log)
  }

  async getProgramHistory(): Promise<GeneratedProgramRecord[]> {
    return readJson<GeneratedProgramRecord[]>(KEYS.programHistory, [])
  }

  async appendProgramRecord(record: GeneratedProgramRecord): Promise<void> {
    const history = await this.getProgramHistory()
    history.push(record)

    // Keep only the last N records per combo to bound storage growth,
    // while still giving the generator enough history to avoid repeats.
    const byCombo = new Map<string, GeneratedProgramRecord[]>()
    for (const rec of history) {
      const list = byCombo.get(rec.comboKey) ?? []
      list.push(rec)
      byCombo.set(rec.comboKey, list)
    }

    const trimmed: GeneratedProgramRecord[] = []
    for (const list of byCombo.values()) {
      list.sort((a, b) => a.generatedAt.localeCompare(b.generatedAt))
      trimmed.push(...list.slice(-MAX_PROGRAM_RECORDS_PER_COMBO))
    }

    writeJson(KEYS.programHistory, trimmed)
  }

  async getProgressionState(): Promise<ProgressionState> {
    return readJson<ProgressionState>(KEYS.progressionState, {})
  }

  async saveProgressionState(state: ProgressionState): Promise<void> {
    writeJson(KEYS.progressionState, state)
  }

  async clearAll(): Promise<void> {
    Object.values(KEYS).forEach((key) => localStorage.removeItem(key))
  }
}

export const storage: DataRepository = new LocalStorageRepository()

export async function loadAllData(): Promise<AppData> {
  const [userProfile, workoutLog, programHistory, progressionState] =
    await Promise.all([
      storage.getUserProfile(),
      storage.getWorkoutLog(),
      storage.getProgramHistory(),
      storage.getProgressionState(),
    ])
  return { userProfile, workoutLog, programHistory, progressionState }
}
