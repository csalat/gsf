﻿//******************************************************************************************************
//  FrameBasedRandomValueInputAdapter.cs - Gbtc
//
//  Copyright © 2014, Grid Protection Alliance.  All Rights Reserved.
//
//  Licensed to the Grid Protection Alliance (GPA) under one or more contributor license agreements. See
//  the NOTICE file distributed with this work for additional information regarding copyright ownership.
//  The GPA licenses this file to you under the Eclipse Public License -v 1.0 (the "License"); you may
//  not use this file except in compliance with the License. You may obtain a copy of the License at:
//
//      http://www.opensource.org/licenses/eclipse-1.0.php
//
//  Unless agreed to in writing, the subject software distributed under the License is distributed on an
//  "AS-IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. Refer to the
//  License for the specific language governing permissions and limitations.
//
//  Code Modification History:
//  ----------------------------------------------------------------------------------------------------
//  04/26/2014 - Stephen C. Wills
//       Generated original version of source code.
//
//******************************************************************************************************

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using GSF;
using GSF.TimeSeries;
using GSF.TimeSeries.Adapters;
using Timer = System.Timers.Timer;

namespace TestingAdapters
{
    /// <summary>
    /// Represents a class used to stream frames of measurements with random values meant to simulate a frame-based input source.
    /// </summary>
    [Description("Random Frames: streams frames of measurements with random values meant to simulate a frame-based input source.")]
    public class FrameBasedRandomValueInputAdapter : InputAdapterBase
    {
        #region [ Members ]

        // Constants

        /// <summary>
        /// Default value for the <see cref="PublishRate"/> property.
        /// </summary>
        public const double DefaultPublishRate = 30.0;

        // Fields
        private double m_publishRate;

        private Timer m_timer;
        private long m_lastPublication;

        private bool m_disposed;

        #endregion

        #region [ Properties ]

        /// <summary>
        /// Gets or sets the number of frames generated by the adapter per second.
        /// </summary>
        [ConnectionStringParameter,
        DefaultValue(DefaultPublishRate),
        Description("Defines the number of frames generated by the adapter per second.")]
        public double PublishRate
        {
            get
            {
                return m_publishRate;
            }
            set
            {
                m_publishRate = value;
            }
        }

        /// <summary>
        /// Gets the flag indicating if this adapter supports temporal processing.
        /// </summary>
        public override bool SupportsTemporalProcessing
        {
            get
            {
                return false;
            }
        }

        /// <summary>
        /// Gets flag that determines if the data input connects asynchronously.
        /// </summary>
        /// <remarks>
        /// Derived classes should return true when data input source is connects asynchronously, otherwise return false.
        /// </remarks>
        protected override bool UseAsyncConnect
        {
            get
            {
                return false;
            }
        }

        #endregion

        #region [ Methods ]

        /// <summary>
        /// Initializes <see cref="MovingValueInputAdapter"/>.
        /// </summary>
        public override void Initialize()
        {
            Dictionary<string, string> settings;
            string setting;

            base.Initialize();
            settings = Settings;

            if (!settings.TryGetValue("publishRate", out setting) || !double.TryParse(setting, out m_publishRate))
                m_publishRate = DefaultPublishRate;

            if (m_publishRate <= 0.0D)
                throw new InvalidOperationException(string.Format("publishRate({0}) must be greater than zero", m_publishRate));
        }

        /// <summary>
        /// Gets a short one-line status of this <see cref="MovingValueInputAdapter"/>.
        /// </summary>
        /// <param name="maxLength">Maximum number of available characters for display.</param>
        /// <returns>
        /// A short one-line summary of the current status of this <see cref="MovingValueInputAdapter"/>.
        /// </returns>
        public override string GetShortStatus(int maxLength)
        {
            return string.Format("{0} random values generated so far...", ProcessedMeasurements).CenterText(maxLength);
        }

        /// <summary>
        /// Attempts to connect to data input source.
        /// </summary>
        protected override void AttemptConnection()
        {
            m_lastPublication = ToPublicationTime(DateTime.UtcNow.Ticks);

            if ((object)m_timer == null)
            {
                m_timer = new Timer();
                m_timer.AutoReset = false;
                m_timer.Elapsed += (sender, args) => PublishFrames();
                ScheduleNextFramePublication();
            }
        }

        /// <summary>
        /// Attempts to disconnect from data input source.
        /// </summary>
        protected override void AttemptDisconnection()
        {
            if ((object)m_timer != null)
            {
                m_timer.Stop();
                m_timer.Dispose();
                m_timer = null;
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by the <see cref="MovingValueInputAdapter"/> object and optionally releases the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to release only unmanaged resources.</param>
        protected override void Dispose(bool disposing)
        {
            if (!m_disposed)
            {
                try
                {
                    if (disposing)
                    {
                        if ((object)m_timer != null)
                        {
                            m_timer.Stop();
                            m_timer.Dispose();
                            m_timer = null;
                        }
                    }
                }
                finally
                {
                    m_disposed = true;          // Prevent duplicate dispose.
                    base.Dispose(disposing);    // Call base class Dispose().
                }
            }
        }

        private void ScheduleNextFramePublication()
        {
            long now = DateTime.UtcNow.Ticks;
            long nextPublication = GetNextPublicationTime(m_lastPublication);
            double delta = Ticks.ToMilliseconds(nextPublication - now);

            if (!Enabled)
                return;

            if (delta < 1.0D)
            {
                ThreadPool.QueueUserWorkItem(state => PublishFrames());
            }
            else if ((object)m_timer != null)
            {
                m_timer.Interval = delta;
                m_timer.Start();
            }
        }

        private void PublishFrames()
        {
            long now = DateTime.UtcNow.Ticks;
            long nextPublication = GetNextPublicationTime(m_lastPublication);

            while (nextPublication < now)
            {
                OnNewMeasurements(OutputMeasurements
                    .Select((measurement, index) => Measurement.Clone(measurement, ThreadLocalGenerator.Value.NextDouble(), nextPublication))
                    .ToList<IMeasurement>());

                m_lastPublication = nextPublication;
                nextPublication = GetNextPublicationTime(m_lastPublication);
            }

            ScheduleNextFramePublication();
        }

        private long GetNextPublicationTime(long time)
        {
            double interval = Ticks.PerSecond / m_publishRate;
            long nextTime = (long)Math.Round(time + interval);
            return ToPublicationTime(nextTime);
        }

        private long ToPublicationTime(long time)
        {
            double interval = Ticks.PerSecond / m_publishRate;
            long seconds = time / Ticks.PerSecond;
            long subseconds = time % Ticks.PerSecond;
            long index = (long)Math.Round(subseconds / interval);
            return (seconds * Ticks.PerSecond) + (long)Math.Round(index * interval);
        }

        #endregion

        #region [ Static ]

        // Static Fields
        private static readonly Random Generator = new Random();
        private static readonly ThreadLocal<Random> ThreadLocalGenerator = new ThreadLocal<Random>(CreateGenerator);

        // Static Methods
        private static Random CreateGenerator()
        {
            lock (Generator)
            {
                return new Random(Generator.Next());
            }
        }

        #endregion
    }
}
