import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from 'react'
import { loadAllData, storage } from '../lib/storage'
import type {
  GeneratedProgramRecord,
  ProgressionState,
  UserProfile,
  WorkoutLogEntry,
} from '../types'

interface AppDataContextValue {
  loading: boolean
  userProfile: UserProfile | null
  workoutLog: WorkoutLogEntry[]
  programHistory: GeneratedProgramRecord[]
  progressionState: ProgressionState
  refresh: () => Promise<void>
  saveUserProfile: (profile: UserProfile) => Promise<void>
  appendWorkoutLog: (entry: WorkoutLogEntry) => Promise<void>
  appendProgramRecord: (record: GeneratedProgramRecord) => Promise<void>
  saveProgressionState: (state: ProgressionState) => Promise<void>
}

const AppDataContext = createContext<AppDataContextValue | null>(null)

export function AppDataProvider({ children }: { children: ReactNode }) {
  const [loading, setLoading] = useState(true)
  const [userProfile, setUserProfile] = useState<UserProfile | null>(null)
  const [workoutLog, setWorkoutLog] = useState<WorkoutLogEntry[]>([])
  const [programHistory, setProgramHistory] = useState<GeneratedProgramRecord[]>([])
  const [progressionState, setProgressionState] = useState<ProgressionState>({})

  const refresh = useCallback(async () => {
    const data = await loadAllData()
    setUserProfile(data.userProfile)
    setWorkoutLog(data.workoutLog)
    setProgramHistory(data.programHistory)
    setProgressionState(data.progressionState)
    setLoading(false)
  }, [])

  useEffect(() => {
    refresh()
  }, [refresh])

  const saveUserProfile = useCallback(async (profile: UserProfile) => {
    await storage.saveUserProfile(profile)
    setUserProfile(profile)
  }, [])

  const appendWorkoutLog = useCallback(async (entry: WorkoutLogEntry) => {
    await storage.appendWorkoutLog(entry)
    setWorkoutLog((prev) => [...prev, entry])
  }, [])

  const appendProgramRecord = useCallback(async (record: GeneratedProgramRecord) => {
    await storage.appendProgramRecord(record)
    const history = await storage.getProgramHistory()
    setProgramHistory(history)
  }, [])

  const saveProgressionState = useCallback(async (state: ProgressionState) => {
    await storage.saveProgressionState(state)
    setProgressionState(state)
  }, [])

  return (
    <AppDataContext.Provider
      value={{
        loading,
        userProfile,
        workoutLog,
        programHistory,
        progressionState,
        refresh,
        saveUserProfile,
        appendWorkoutLog,
        appendProgramRecord,
        saveProgressionState,
      }}
    >
      {children}
    </AppDataContext.Provider>
  )
}

export function useAppData(): AppDataContextValue {
  const ctx = useContext(AppDataContext)
  if (!ctx) throw new Error('useAppData must be used within AppDataProvider')
  return ctx
}
