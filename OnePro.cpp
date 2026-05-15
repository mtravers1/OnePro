#region Using declarations
using System;
using System.ComponentModel;
using System.ComponentModel.DataAnnotations;
using NinjaTrader.Cbi;
using NinjaTrader.Data;
using NinjaTrader.Gui.Tools;
using NinjaTrader.NinjaScript;
using NinjaTrader.Core.FloatingPoint;
using NinjaTrader.NinjaScript.Indicators;
#endregion

namespace NinjaTrader.NinjaScript.Strategies.Premium_1PAlgos
{
    public class NewOneProv4 : Strategy
    {
        #region Licensing
        private string[] productCodes = new string[] { "P_0226", "P_0229" };
        private bool licensed = false;
        #endregion

        // ── Indicators ────────────────────────────────────────────────────────────
        private Bollinger BollingerEntry, BollingerExit;
        private DM       DM1;
        private NinjaTrader.NinjaScript.Indicators.EMA Ema200;
        private ATR      ATR1;
        private VOL      VOL1;

        // ── Trade-state tracking ──────────────────────────────────────────────────
        private double entryPrice      = 0;
        private int    barsSinceEntry  = 0;
        private bool   breakevenActive = false;

        // ── Daily P&L tracking ───────────────────────────────────────────────────
        private double dailyPnL         = 0;
        private int    lastDailyBarCount = -1;

        public NewOneProv4()
        {
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  STATE CHANGE
        // ═══════════════════════════════════════════════════════════════════════════
        protected override void OnStateChange()
        {
            if (State == State.SetDefaults)
            {
                Description = @"NQ-optimized mean reversion with dynamic exits, breakeven management, volatility filters, and two configurable mid-day pause windows";
                Name        = "NewOneProv4";
                Calculate   = Calculate.OnBarClose;

                EntriesPerDirection = 1;
                EntryHandling       = EntryHandling.AllEntries;

                IsExitOnSessionCloseStrategy = true;
                ExitOnSessionCloseSeconds    = 30;
                StartBehavior                = StartBehavior.WaitUntilFlat;
                TimeInForce                  = TimeInForce.Gtc;
                BarsRequiredToTrade          = 20;

                Contracts = 2;

                UpperShortEntry = 14;
                LowerLongEntry  = 14;

                StopLossMultiplier = 1.1;
                BaseStopLoss       = 200;

                FirstTargetTicks   = 40;
                FirstTargetPercent = 50;
                SecondTargetTicks  = 110;

                UseTrailingStop     = true;
                TrailingStopTicks   = 200;
                TrailingOffsetTicks = 15;

                UseBreakeven          = true;
                BreakevenTriggerTicks = 10;
                BreakevenOffsetTicks  = 50;

                MaxADX = 50;
                MinADX = 20;

                EmaLength = 100;

                MinVolumeMultiplier = 0.5;

                UseVolatilityFilter = true;
                MinATR = 10.0;
                MaxATR = 15.0;

                TradeMonday    = true;
                TradeTuesday   = true;
                TradeWednesday = true;
                TradeThursday  = true;
                TradeFriday    = true;

                AllowLongs  = true;
                AllowShorts = true;

                TradeStartHour   = 10;
                TradeStartMinute = 30;
                TradeEndHour     = 14;
                TradeEndMinute   = 30;

                AvoidFirstMinutes = 15;

                UseDailyLossLimit = true;
                MaxDailyLossTicks = 200;

                MaxBarsInTrade = 15;

                // ── Mid-day pause window 1 ───────────────────────────────────────
                UseMidDayPause    = true;
                PauseStartHour    = 13;
                PauseStartMinute  =  0;
                PauseEndHour      = 14;
                PauseEndMinute    =  0;

                // ── Mid-day pause window 2 ───────────────────────────────────────
                UseMidDayPause2   = true;
                Pause2StartHour   = 11;
                Pause2StartMinute =  0;
                Pause2EndHour     = 12;
                Pause2EndMinute   =  0;
            }
            else if (State == State.Configure)
            {
                entryPrice        = 0;
                barsSinceEntry    = 0;
                breakevenActive   = false;

                dailyPnL          = 0;
                lastDailyBarCount = -1;
            }
            else if (State == State.DataLoaded)
            {
                licensed = new AgileLicensing(this).ValidateLicense(productCodes);
                if (!licensed)
                    Print($"{Time[0]} {Instrument?.FullName} — License validation failed. Strategy will not trade.");

                BollingerEntry = Bollinger(Close, 2, 14);
                BollingerExit  = Bollinger(Close, 3, 14);

                DM1    = DM(Close, 14);
                Ema200 = EMA(Close, EmaLength);
                ATR1   = ATR(Close, 14);
                VOL1   = VOL(Close);

                AddChartIndicator(BollingerEntry);
                AddChartIndicator(Ema200);
                AddChartIndicator(ATR1);
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  BAR UPDATE
        // ═══════════════════════════════════════════════════════════════════════════
        protected override void OnBarUpdate()
        {
            if (!licensed)
                return;

            if (BarsInProgress != 0 || CurrentBars[0] < Math.Max(20, EmaLength))
                return;

            if (double.IsNaN(Ema200.Value[0]) || double.IsNaN(ATR1.Value[0]))
                return;

            TimeSpan currentTime = Times[0][0].TimeOfDay;

            if (!IsTradingDay())
                return;

            if (UseDailyLossLimit && IsDailyLossLimitReached())
                return;

            // ── In-trade management ───────────────────────────────────────────────
            if (Position.MarketPosition != MarketPosition.Flat)
            {
                barsSinceEntry++;

                if (UseBreakeven && !breakevenActive)
                    CheckAndApplyBreakeven();

                if (barsSinceEntry >= MaxBarsInTrade)
                {
                    if (Position.MarketPosition == MarketPosition.Long)
                        ExitLong("Stale Exit", "LongEntry");
                    else if (Position.MarketPosition == MarketPosition.Short)
                        ExitShort("Stale Exit", "ShortEntry");

                    ResetTradeTracking();
                    return;
                }
            }

            // ── Entry logic ───────────────────────────────────────────────────────
            TimeSpan tradeStart = new TimeSpan(TradeStartHour, TradeStartMinute, 0)
                                      .Add(TimeSpan.FromMinutes(AvoidFirstMinutes));
            TimeSpan tradeEnd   = new TimeSpan(TradeEndHour, TradeEndMinute, 0);

            // ── Mid-day pause: block new entries during either pause window ─────
            bool inPauseWindow = false;
            if (UseMidDayPause)
            {
                TimeSpan pauseStart = new TimeSpan(PauseStartHour, PauseStartMinute, 0);
                TimeSpan pauseEnd   = new TimeSpan(PauseEndHour,   PauseEndMinute,   0);
                inPauseWindow = (currentTime >= pauseStart && currentTime < pauseEnd);
            }
            if (!inPauseWindow && UseMidDayPause2)
            {
                TimeSpan pause2Start = new TimeSpan(Pause2StartHour, Pause2StartMinute, 0);
                TimeSpan pause2End   = new TimeSpan(Pause2EndHour,   Pause2EndMinute,   0);
                inPauseWindow = (currentTime >= pause2Start && currentTime < pause2End);
            }

            if (currentTime >= tradeStart && currentTime <= tradeEnd
                && !inPauseWindow
                && Position.MarketPosition == MarketPosition.Flat)
            {
                if (!PassesQualityFilters())
                    return;

                if (AllowShorts
                    && Close[0] < Ema200.Value[0]
                    && CrossAbove(Close, BollingerEntry.Upper, 1)
                    && DM1.ADXPlot[0] >= MinADX && DM1.ADXPlot[0] <= MaxADX)
                {
                    EnterShortWithManagement();
                }

                if (AllowLongs
                    && Close[0] > Ema200.Value[0]
                    && CrossAbove(Close, BollingerEntry.Lower, 1)
                    && DM1.ADXPlot[0] >= MinADX && DM1.ADXPlot[0] <= MaxADX)
                {
                    EnterLongWithManagement();
                }
            }

            // ── Bollinger mean-reversion exit ─────────────────────────────────────
            // NOTE: exits are NOT blocked during the pause window — open positions
            // continue to be managed and can exit normally at any time.
            if (Position.MarketPosition == MarketPosition.Short
                && CrossBelow(Close, BollingerExit.Upper, 1))
            {
                ExitShort("BB Exit", "ShortEntry");
            }

            if (Position.MarketPosition == MarketPosition.Long
                && CrossAbove(Close, BollingerExit.Upper, 1))
            {
                ExitLong("BB Exit", "LongEntry");
            }

            // ── End-of-day hard exit ──────────────────────────────────────────────
            if (currentTime >= new TimeSpan(16, 55, 0))
            {
                if (Position.MarketPosition == MarketPosition.Long)
                    ExitLong("EOD Exit", "LongEntry");
                else if (Position.MarketPosition == MarketPosition.Short)
                    ExitShort("EOD Exit", "ShortEntry");
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  ENTRY HELPERS
        // ═══════════════════════════════════════════════════════════════════════════
        private void EnterShortWithManagement()
        {
            entryPrice      = Close[0];
            barsSinceEntry  = 0;
            breakevenActive = false;

            double atrTicks  = ATR1.Value[0] / Instrument.MasterInstrument.TickSize;
            int    stopTicks = (int)Math.Max(BaseStopLoss, atrTicks * StopLossMultiplier);

            EnterShort(Contracts, "ShortEntry");
            SetStopLoss("ShortEntry", CalculationMode.Ticks, stopTicks, false);

            if (FirstTargetPercent > 0 && FirstTargetPercent < 100)
            {
                SetProfitTarget("ShortEntry", CalculationMode.Ticks, FirstTargetTicks, false);
                if (UseTrailingStop)
                    SetTrailStop("ShortEntry", CalculationMode.Ticks, TrailingOffsetTicks, false);
            }
            else
            {
                SetProfitTarget("ShortEntry", CalculationMode.Ticks, SecondTargetTicks, false);
            }

            Print(String.Format("{0} SHORT entry @ {1} | Stop {2} ticks | Target1 {3} ticks",
                Times[0][0], entryPrice, stopTicks, FirstTargetTicks));
        }

        private void EnterLongWithManagement()
        {
            entryPrice      = Close[0];
            barsSinceEntry  = 0;
            breakevenActive = false;

            double atrTicks  = ATR1.Value[0] / Instrument.MasterInstrument.TickSize;
            int    stopTicks = (int)Math.Max(BaseStopLoss, atrTicks * StopLossMultiplier);

            EnterLong(Contracts, "LongEntry");
            SetStopLoss("LongEntry", CalculationMode.Ticks, stopTicks, false);

            if (FirstTargetPercent > 0 && FirstTargetPercent < 100)
            {
                SetProfitTarget("LongEntry", CalculationMode.Ticks, FirstTargetTicks, false);
                if (UseTrailingStop)
                    SetTrailStop("LongEntry", CalculationMode.Ticks, TrailingOffsetTicks, false);
            }
            else
            {
                SetProfitTarget("LongEntry", CalculationMode.Ticks, SecondTargetTicks, false);
            }

            Print(String.Format("{0} LONG  entry @ {1} | Stop {2} ticks | Target1 {3} ticks",
                Times[0][0], entryPrice, stopTicks, FirstTargetTicks));
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  BREAKEVEN MANAGEMENT
        // ═══════════════════════════════════════════════════════════════════════════
        private void CheckAndApplyBreakeven()
        {
            if (entryPrice <= 0)
                return;

            double tick = Instrument.MasterInstrument.TickSize;

            if (Position.MarketPosition == MarketPosition.Long)
            {
                double triggerPrice = entryPrice + BreakevenTriggerTicks * tick;
                if (Close[0] >= triggerPrice)
                {
                    double beStopPrice = entryPrice + BreakevenOffsetTicks * tick;
                    SetStopLoss("LongEntry", CalculationMode.Price, beStopPrice, false);
                    breakevenActive = true;
                    Print(String.Format("{0} BE applied LONG  | Entry {1} → Stop moved to {2}",
                        Times[0][0], entryPrice, beStopPrice));
                }
            }
            else if (Position.MarketPosition == MarketPosition.Short)
            {
                double triggerPrice = entryPrice - BreakevenTriggerTicks * tick;
                if (Close[0] <= triggerPrice)
                {
                    double beStopPrice = entryPrice - BreakevenOffsetTicks * tick;
                    SetStopLoss("ShortEntry", CalculationMode.Price, beStopPrice, false);
                    breakevenActive = true;
                    Print(String.Format("{0} BE applied SHORT | Entry {1} → Stop moved to {2}",
                        Times[0][0], entryPrice, beStopPrice));
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  QUALITY FILTERS
        // ═══════════════════════════════════════════════════════════════════════════
        private bool PassesQualityFilters()
        {
            if (UseVolatilityFilter)
            {
                double atr = ATR1.Value[0];
                if (atr < MinATR || atr > MaxATR)
                    return false;
            }

            if (MinVolumeMultiplier > 0)
            {
                double avgVol = SMA(VOL1, 20)[0];
                if (avgVol > 0 && VOL1[0] < avgVol * MinVolumeMultiplier)
                    return false;
            }

            return true;
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  DAILY LOSS LIMIT
        // ═══════════════════════════════════════════════════════════════════════════
        private bool IsDailyLossLimitReached()
        {
            if (CurrentBars[0] != lastDailyBarCount)
            {
                dailyPnL       = 0;
                DateTime today = Times[0][0].Date;

                foreach (Trade trade in SystemPerformance.AllTrades)
                {
                    if (trade.Exit.Time.Date == today)
                        dailyPnL += trade.ProfitCurrency;
                }

                lastDailyBarCount = CurrentBars[0];
            }

            double tickValue2 = Instrument.MasterInstrument.PointValue
                                * Instrument.MasterInstrument.TickSize;

            if (tickValue2 > 0)
            {
                double todayTicks = dailyPnL / tickValue2;
                if (todayTicks <= -MaxDailyLossTicks)
                {
                    Print(String.Format("{0} Daily loss limit hit: {1:F0} ticks. No new trades.",
                        Times[0][0], todayTicks));
                    return true;
                }
            }

            return false;
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  HELPERS
        // ═══════════════════════════════════════════════════════════════════════════
        private void ResetTradeTracking()
        {
            entryPrice      = 0;
            barsSinceEntry  = 0;
            breakevenActive = false;
        }

        private bool IsTradingDay()
        {
            DayOfWeek day = Times[0][0].DayOfWeek;
            return (day == DayOfWeek.Monday    && TradeMonday)
                || (day == DayOfWeek.Tuesday   && TradeTuesday)
                || (day == DayOfWeek.Wednesday && TradeWednesday)
                || (day == DayOfWeek.Thursday  && TradeThursday)
                || (day == DayOfWeek.Friday    && TradeFriday);
        }

        // ═══════════════════════════════════════════════════════════════════════════
        //  PROPERTIES
        // ═══════════════════════════════════════════════════════════════════════════
        #region Properties

        // ── Position sizing ────────────────────────────────────────────────────────
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Contracts", Order = 1, GroupName = "Position Sizing")]
        public int Contracts { get; set; }

        // ── Legacy params ──────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Upper Short Entry", Order = 1, GroupName = "Legacy")]
        public int UpperShortEntry { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Lower Long Entry", Order = 2, GroupName = "Legacy")]
        public int LowerLongEntry { get; set; }

        // ── Risk management ────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0.1, 10.0)]
        [Display(Name = "Stop Loss ATR Multiplier", Order = 1, GroupName = "Risk Management")]
        public double StopLossMultiplier { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Base Stop Loss (Ticks)", Order = 2, GroupName = "Risk Management")]
        public int BaseStopLoss { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Daily Loss Limit", Order = 3, GroupName = "Risk Management")]
        public bool UseDailyLossLimit { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Max Daily Loss (Ticks)", Order = 4, GroupName = "Risk Management")]
        public int MaxDailyLossTicks { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Max Bars In Trade", Order = 5, GroupName = "Risk Management")]
        public int MaxBarsInTrade { get; set; }

        // ── Breakeven ──────────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Breakeven", Order = 1, GroupName = "Breakeven Management")]
        public bool UseBreakeven { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Breakeven Trigger (Ticks)", Order = 2, GroupName = "Breakeven Management")]
        public int BreakevenTriggerTicks { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, int.MaxValue)]
        [Display(Name = "Breakeven Offset (Ticks)", Order = 3, GroupName = "Breakeven Management")]
        public int BreakevenOffsetTicks { get; set; }

        // ── Profit targets ─────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "First Target (Ticks)", Order = 1, GroupName = "Profit Targets")]
        public int FirstTargetTicks { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 100)]
        [Display(Name = "First Target % of Position", Order = 2, GroupName = "Profit Targets")]
        public int FirstTargetPercent { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Second Target (Ticks)", Order = 3, GroupName = "Profit Targets")]
        public int SecondTargetTicks { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Trailing Stop", Order = 4, GroupName = "Profit Targets")]
        public bool UseTrailingStop { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Trailing Stop Trigger (Ticks)", Order = 5, GroupName = "Profit Targets")]
        public int TrailingStopTicks { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "Trailing Stop Offset (Ticks)", Order = 6, GroupName = "Profit Targets")]
        public int TrailingOffsetTicks { get; set; }

        // ── Entry filters ──────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, 50)]
        [Display(Name = "Maximum ADX", Order = 1, GroupName = "Entry Filters")]
        public int MaxADX { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, 50)]
        [Display(Name = "Minimum ADX", Order = 2, GroupName = "Entry Filters")]
        public int MinADX { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(1, int.MaxValue)]
        [Display(Name = "EMA Length", Order = 3, GroupName = "Entry Filters")]
        public int EmaLength { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Volatility Filter", Order = 4, GroupName = "Entry Filters")]
        public bool UseVolatilityFilter { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0.1, 200.0)]
        [Display(Name = "Minimum ATR (Points)", Order = 5, GroupName = "Entry Filters")]
        public double MinATR { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0.1, 200.0)]
        [Display(Name = "Maximum ATR (Points)", Order = 6, GroupName = "Entry Filters")]
        public double MaxATR { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0.1, 2.0)]
        [Display(Name = "Min Volume Multiplier", Order = 7, GroupName = "Entry Filters")]
        public double MinVolumeMultiplier { get; set; }

        // ── Time filters ───────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Trade Start Hour", Order = 1, GroupName = "Time Filters")]
        public int TradeStartHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Trade Start Minute", Order = 2, GroupName = "Time Filters")]
        public int TradeStartMinute { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Trade End Hour", Order = 3, GroupName = "Time Filters")]
        public int TradeEndHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Trade End Minute", Order = 4, GroupName = "Time Filters")]
        public int TradeEndMinute { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 60)]
        [Display(Name = "Avoid First X Minutes", Order = 5, GroupName = "Time Filters")]
        public int AvoidFirstMinutes { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Trade Monday",    Order = 6, GroupName = "Time Filters")]
        public bool TradeMonday    { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Trade Tuesday",   Order = 7, GroupName = "Time Filters")]
        public bool TradeTuesday   { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Trade Wednesday", Order = 8, GroupName = "Time Filters")]
        public bool TradeWednesday { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Trade Thursday",  Order = 9, GroupName = "Time Filters")]
        public bool TradeThursday  { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Trade Friday",    Order = 10, GroupName = "Time Filters")]
        public bool TradeFriday    { get; set; }

        // ── Mid-day pause 1 ────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Mid-Day Pause 1", Order = 1, GroupName = "Mid-Day Pause 1")]
        public bool UseMidDayPause { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Pause 1 Start Hour", Order = 2, GroupName = "Mid-Day Pause 1")]
        public int PauseStartHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Pause 1 Start Minute", Order = 3, GroupName = "Mid-Day Pause 1")]
        public int PauseStartMinute { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Pause 1 End Hour", Order = 4, GroupName = "Mid-Day Pause 1")]
        public int PauseEndHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Pause 1 End Minute", Order = 5, GroupName = "Mid-Day Pause 1")]
        public int PauseEndMinute { get; set; }

        // ── Mid-day pause 2 ────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Use Mid-Day Pause 2", Order = 1, GroupName = "Mid-Day Pause 2")]
        public bool UseMidDayPause2 { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Pause 2 Start Hour", Order = 2, GroupName = "Mid-Day Pause 2")]
        public int Pause2StartHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Pause 2 Start Minute", Order = 3, GroupName = "Mid-Day Pause 2")]
        public int Pause2StartMinute { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 23)]
        [Display(Name = "Pause 2 End Hour", Order = 4, GroupName = "Mid-Day Pause 2")]
        public int Pause2EndHour { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Range(0, 59)]
        [Display(Name = "Pause 2 End Minute", Order = 5, GroupName = "Mid-Day Pause 2")]
        public int Pause2EndMinute { get; set; }

        // ── Direction ──────────────────────────────────────────────────────────────
        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Allow Longs",  Order = 1, GroupName = "Direction")]
        public bool AllowLongs  { get; set; }

        [Browsable(true)]
        [NinjaScriptProperty]
        [Display(Name = "Allow Shorts", Order = 2, GroupName = "Direction")]
        public bool AllowShorts { get; set; }

        #endregion
    }
}