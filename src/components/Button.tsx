import type { ButtonHTMLAttributes, ReactNode } from 'react'

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'ghost'
  children: ReactNode
}

const VARIANT_CLASSES: Record<NonNullable<ButtonProps['variant']>, string> = {
  primary:
    'bg-emerald-500 text-slate-950 hover:bg-emerald-400 active:bg-emerald-600 shadow-lg shadow-emerald-500/20',
  secondary:
    'bg-slate-800 text-slate-100 hover:bg-slate-700 active:bg-slate-900 border border-slate-700',
  ghost: 'bg-transparent text-slate-300 hover:text-slate-100',
}

export function Button({ variant = 'primary', className = '', children, ...rest }: ButtonProps) {
  return (
    <button
      className={`w-full rounded-2xl px-5 py-4 text-base font-semibold transition-colors disabled:opacity-40 disabled:pointer-events-none ${VARIANT_CLASSES[variant]} ${className}`}
      {...rest}
    >
      {children}
    </button>
  )
}
