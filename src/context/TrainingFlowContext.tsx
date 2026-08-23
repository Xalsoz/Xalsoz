import { createContext, useContext, useState, type ReactNode } from 'react'
import type { GeneratedProgram } from '../lib/programGenerator'
import type { EnergyLevel, Environment } from '../types'

interface TrainingFlowValue {
  environment: Environment | null
  energyLevel: EnergyLevel | null
  program: GeneratedProgram | null
  setEnvironment: (env: Environment) => void
  setEnergyLevel: (level: EnergyLevel) => void
  setProgram: (program: GeneratedProgram) => void
  reset: () => void
}

const TrainingFlowContext = createContext<TrainingFlowValue | null>(null)

export function TrainingFlowProvider({ children }: { children: ReactNode }) {
  const [environment, setEnvironment] = useState<Environment | null>(null)
  const [energyLevel, setEnergyLevel] = useState<EnergyLevel | null>(null)
  const [program, setProgram] = useState<GeneratedProgram | null>(null)

  const reset = () => {
    setEnvironment(null)
    setEnergyLevel(null)
    setProgram(null)
  }

  return (
    <TrainingFlowContext.Provider
      value={{ environment, energyLevel, program, setEnvironment, setEnergyLevel, setProgram, reset }}
    >
      {children}
    </TrainingFlowContext.Provider>
  )
}

export function useTrainingFlow(): TrainingFlowValue {
  const ctx = useContext(TrainingFlowContext)
  if (!ctx) throw new Error('useTrainingFlow must be used within TrainingFlowProvider')
  return ctx
}
