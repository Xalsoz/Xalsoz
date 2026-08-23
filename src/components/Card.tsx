import type { HTMLAttributes, ReactNode } from 'react'

interface CardProps extends HTMLAttributes<HTMLDivElement> {
  children: ReactNode
}

export function Card({ className = '', children, ...rest }: CardProps) {
  return (
    <div
      className={`rounded-2xl border border-slate-800 bg-slate-900/60 p-4 ${className}`}
      {...rest}
    >
      {children}
    </div>
  )
}
