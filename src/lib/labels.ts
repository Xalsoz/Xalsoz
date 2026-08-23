import type { EnergyLevel, Environment } from '../types'

export const ENVIRONMENT_LABELS: Record<Environment, string> = {
  'home-no-bar': 'Дома без турника',
  'home-bar': 'Есть турник/брусья дома',
  park: 'Парк со спортплощадкой',
  gym: 'Зал',
  'no-equipment': 'Вообще без инвентаря',
}

export const ENVIRONMENT_ICONS: Record<Environment, string> = {
  'home-no-bar': '🏠',
  'home-bar': '🧗',
  park: '🌳',
  gym: '🏋️',
  'no-equipment': '🧍',
}

export const ENERGY_LABELS: Record<EnergyLevel, string> = {
  low: 'Низкий',
  medium: 'Средний',
  high: 'Высокий',
}

export const UNIT_LABELS = {
  reps: 'повторений',
  seconds: 'сек',
} as const
