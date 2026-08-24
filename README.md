# Гибкая калистеника

Mobile-first PWA для калистеники без жёсткого расписания. Пользователь сам
решает, когда заниматься — приложение подстраивает программу под среду
(дом/турник/парк/зал/без инвентаря) и уровень энергии на сегодня, ротирует
упражнения и постепенно усложняет прогрессии.

## Стек

- React + TypeScript, Vite
- Tailwind CSS 4
- react-router-dom
- localStorage через `DataRepository` (`src/lib/storage.ts`) — интерфейс,
  за которым позже можно спрятать реальный бэкенд
- vite-plugin-pwa (manifest + service worker)

## Разработка

```bash
npm install
npm run dev      # http://localhost:5173
npm run build    # прод-сборка + PWA-ассеты в dist/
npm run lint
```

## Структура

- `src/types` — доменные типы (`UserProfile`, `WorkoutLogEntry`, `ExerciseDef`, …)
- `src/lib/exercisePool.ts` — захардкоженный справочник упражнений с тегами
  среды/сложности/группы мышц и цепочками прогрессий
- `src/lib/programGenerator.ts` — генерация программы под
  среда × энергия × текущий прогресс, ротация без повторов подряд
- `src/lib/progress.ts` — сводки прогресса, стрик низкой энергии
- `src/context` — `AppDataProvider` (данные из localStorage) и
  `TrainingFlowProvider` (состояние визарда «хочу позаниматься»)
- `src/screens` — экраны: онбординг, главный, выбор среды/энергии,
  программа, форма результатов, прогресс, история
