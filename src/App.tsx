import type { ReactNode } from 'react'
import { Navigate, Route, Routes } from 'react-router-dom'
import { AppDataProvider, useAppData } from './context/AppDataContext'
import { TrainingFlowProvider } from './context/TrainingFlowContext'
import { EnergySelect } from './screens/EnergySelect'
import { EnvironmentSelect } from './screens/EnvironmentSelect'
import { History } from './screens/History'
import { Home } from './screens/Home'
import { Onboarding } from './screens/Onboarding'
import { Progress } from './screens/Progress'
import { WorkoutComplete } from './screens/WorkoutComplete'
import { WorkoutProgram } from './screens/WorkoutProgram'

function RequireProfile({ children }: { children: ReactNode }) {
  const { loading, userProfile } = useAppData()
  if (loading) return null
  if (!userProfile) return <Navigate to="/onboarding" replace />
  return <>{children}</>
}

function AppRoutes() {
  const { loading } = useAppData()
  if (loading) return null

  return (
    <Routes>
      <Route path="/onboarding" element={<Onboarding />} />
      <Route
        path="/"
        element={
          <RequireProfile>
            <Home />
          </RequireProfile>
        }
      />
      <Route
        path="/train/environment"
        element={
          <RequireProfile>
            <EnvironmentSelect />
          </RequireProfile>
        }
      />
      <Route
        path="/train/energy"
        element={
          <RequireProfile>
            <EnergySelect />
          </RequireProfile>
        }
      />
      <Route
        path="/train/program"
        element={
          <RequireProfile>
            <WorkoutProgram />
          </RequireProfile>
        }
      />
      <Route
        path="/train/complete"
        element={
          <RequireProfile>
            <WorkoutComplete />
          </RequireProfile>
        }
      />
      <Route
        path="/progress"
        element={
          <RequireProfile>
            <Progress />
          </RequireProfile>
        }
      />
      <Route
        path="/history"
        element={
          <RequireProfile>
            <History />
          </RequireProfile>
        }
      />
      <Route path="*" element={<Navigate to="/" replace />} />
    </Routes>
  )
}

function App() {
  return (
    <AppDataProvider>
      <TrainingFlowProvider>
        <AppRoutes />
      </TrainingFlowProvider>
    </AppDataProvider>
  )
}

export default App
