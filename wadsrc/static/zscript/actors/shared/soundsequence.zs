/*
** soundsequence.zs
**
** Actors for independently playing sound sequences in a map.
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
** A SoundSequence actor has two modes of operation:
**
**   1. If the sound sequence assigned to it has a slot, then a separate
**      SoundSequenceSlot actor is spawned (if not already present), and
**      this actor's sound sequence is added to its list of choices. This
**      actor is then destroyed, never to be heard from again. The sound
**      sequence for the slot is automatically played on the new
**      SoundSequenceSlot actor, and it should at some point execute the
**      randomsequence command so that it can pick one of the other
**      sequences to play. The slot sequence should also end with restart
**      so that more than one sequence will have a chance to play.
**
**      In this mode, it is very much like world $ambient sounds defined
**      in SNDINFO but more flexible.
**
**   2. If the sound sequence assigned to it has no slot, then it will play
**      the sequence when activated and cease playing the sequence when
**      deactivated.
**
**      In this mode, it is very much like point $ambient sounds defined
**      in SNDINFO but more flexible.
**
** To assign a sound sequence, set the SoundSequence's first argument to
** the ID of the corresponding environment sequence you want to use. If
** that sequence is a multiple-choice sequence, then the second argument
** selects which choice it picks.
*/

class AmbientSound : Actor
{
	// Selaco: deferred activation and linked (synchronized) looping ambient sounds.
	// Only used when SelacoMode is set; other games keep the original ambient sound behaviour.
	bool activatedOnce;
	bool user_alwaysPlay;

	AmbientSound masterAmbient;
	Array<AmbientSound> linkedSounds;
	bool hasSearched;

	default
	{
		+NOBLOCKMAP
		+NOSECTOR
		+DONTSPLASH
		+NOTONAUTOMAP
	}

	native void MarkAmbientSounds();
	native void AmbientTick();
	native bool IsLooping();
	native bool StartPlaying();
	native void StopPlaying();
	native double GetAudibleRange();

	native void ActivateAmbient(Actor activator);
	native void DeactivateAmbient(Actor activator);

	override void Activate(Actor activator)
	{
		ActivateAmbient(activator);

		// Selaco: when activated, activate all links
		if (!SelacoMode) return;

		if (masterAmbient && activator != masterAmbient)
		{
			masterAmbient.ActivateAmbient(self);
		}
		else if (!masterAmbient && linkedSounds.size())
		{
			for (int x = 0; x < linkedSounds.size(); x++)
			{
				let ls = linkedSounds[x];
				if (ls && activator != ls)
				{
					ls.ActivateAmbient(self);
				}
				else if (!ls)
				{
					linkedSounds.delete(x);
					x--;
				}
			}
		}
	}

	override void Deactivate(Actor activator)
	{
		DeactivateAmbient(activator);

		// Selaco: when deactivated, deactivate all links
		if (!SelacoMode) return;

		if (masterAmbient && activator != masterAmbient)
		{
			masterAmbient.DeactivateAmbient(self);
		}
		else if (!masterAmbient && linkedSounds.size())
		{
			for (int x = 0; x < linkedSounds.size(); x++)
			{
				let ls = linkedSounds[x];
				if (ls && activator != ls)
				{
					ls.DeactivateAmbient(self);
				}
				else if (!ls)
				{
					linkedSounds.delete(x);
					x--;
				}
			}
		}
	}

	override void BeginPlay ()
	{
		Super.BeginPlay ();
		if (SelacoMode)
		{
			// Selaco: activated by Tick once the map is running
			bDormant = (SpawnFlags & MTF_DORMANT);
		}
		else
		{
			Activate (NULL);
		}
	}

	override void PostBeginPlay()
	{
		Super.PostBeginPlay();

		if (SelacoMode) FindFriends();
	}

	override void MarkPrecacheSounds()
	{
		Super.MarkPrecacheSounds();
		MarkAmbientSounds();
	}

	bool inRangeOfPlayer()
	{
		double range = GetAudibleRange();

		for (int x = 0; x < MAXPLAYERS; x++)
		{
			let mo = players[x].camera && !players[x].camera.player ? players[x].camera : Actor(players[x].mo);
			if (!mo) continue;
			if (Distance2DSquared(mo) < (range * range)) return true;
		}

		for (int x = linkedSounds.size() - 1; x >= 0; x--)
		{
			if (linkedSounds[x] && linkedSounds[x].inRangeOfPlayer()) return true;
		}

		return false;
	}

	// Selaco: callback when a distance-limited sound will resume playing, or start
	// Return FALSE to prevent sound from playing
	virtual bool WillResume()
	{
		// Play linked sounds if exist
		if (!masterAmbient)
		{
			for (int x = linkedSounds.size() - 1; x >= 0; x--)
			{
				let ls = linkedSounds[x];
				if (ls && ls.special1 != INT.max)
				{
					ls.StartPlaying();
				}
			}
		}
		else
		{
			if (masterAmbient.special1 != INT.max) masterAmbient.StartPlaying();
			masterAmbient.WillResume();
			return false;	// Will get played by master
		}

		return true;
	}

	// Selaco: callback when a distance limited sound is no longer hearable and should be halted
	// Return FALSE to cancel halt and continue playing sound
	virtual bool WillHalt()
	{
		if (user_alwaysPlay) return false;	// Don't halt alwaysPlay sounds

		// Check linked sounds and abort if any of them are hearable
		if (masterAmbient && masterAmbient.inRangeOfPlayer())
		{
			return false;
		}
		else if (!masterAmbient && linkedSounds.size() && inRangeOfPlayer())
		{
			return false;
		}

		// Stop linked sounds if necessary
		if (!masterAmbient)
		{
			for (int x = linkedSounds.size() - 1; x >= 0; x--)
			{
				if (linkedSounds[x]) linkedSounds[x].StopPlaying();
			}
		}
		else
		{
			// Halt all sounds from master
			for (int x = masterAmbient.linkedSounds.size() - 1; x >= 0; x--)
			{
				let ls = masterAmbient.linkedSounds[x];
				if (ls && ls != self)
				{
					ls.StopPlaying();
				}
			}
		}

		return true;
	}

	override void Tick()
	{
		Super.Tick();

		if (SelacoMode && !activatedOnce && !bDormant && level.maptime > 5)
		{
			Activate(NULL);
			activatedOnce = true;
		}

		AmbientTick();
	}

	// Selaco: find ambient sounds in range that have the same args[0] and are looping so we can sync their play
	void FindFriends()
	{
		if (masterAmbient || hasSearched || linkedSounds.size() > 0 || !IsLooping()) return;

		AmbientSound as;
		let finder = ThinkerIterator.Create("AmbientSound");
		double range = GetAudibleRange();

		while ((as = AmbientSound(finder.Next())))
		{
			if (as == self || as == masterAmbient) continue;
			if (!as.IsLooping()) continue;
			if (as.args[0] != 0 && as.args[0] == args[0])
			{
				double asRange = as.GetAudibleRange();

				if ((!masterAmbient && as.tid != 0 && as.tid == self.tid) ||
					((range <= 0 || asRange <= 0 || (as.Distance2D(self) < (range + asRange)) && as.bDormant == self.bDormant))
				)
				{
					if (masterAmbient)
					{
						// Check master to see if this sound is already linked
						bool found = false;
						for (int x = masterAmbient.linkedSounds.size() - 1; x >= 0; x--)
						{
							if (masterAmbient.linkedSounds[x] == as) { found = true; break; }
						}
						if (!found)
						{
							masterAmbient.linkedSounds.push(as);
							as.masterAmbient = masterAmbient;
						}
					}
					else
					{
						linkedSounds.push(as);
						as.masterAmbient = self;
					}
				}
			}
		}

		hasSearched = true;
	}
}

class AmbientSoundNoGravity : AmbientSound
{
	default
	{
		+NOGRAVITY
	}
}

class SoundSequenceSlot : Actor
{
	default
	{
		+NOSECTOR
		+NOBLOCKMAP
		+DONTSPLASH
		+NOTONAUTOMAP
	}

	SeqNode sequence;
}

class SoundSequence : Actor
{
	default
	{
		+NOSECTOR
		+NOBLOCKMAP
		+DONTSPLASH
		+NOTONAUTOMAP
	}

	//==========================================================================
	//
	// ASoundSequence :: Destroy
	//
	//==========================================================================

	override void OnDestroy ()
	{
		StopSoundSequence ();
		Super.OnDestroy();
	}

	//==========================================================================
	//
	// ASoundSequence :: PostBeginPlay
	//
	//==========================================================================

	override void PostBeginPlay ()
	{
		Name slot = SeqNode.GetSequenceSlot (args[0], SeqNode.ENVIRONMENT);

		if (slot != 'none')
		{ // This is a slotted sound, so add it to the master for that slot
			SoundSequenceSlot master;
			let locator = ThinkerIterator.Create("SoundSequenceSlot");

			while ((master = SoundSequenceSlot(locator.Next ())))
			{
				if (master.Sequence.GetSequenceName() == slot)
				{
					break;
				}
			}
			if (master == NULL)
			{
				master = SoundSequenceSlot(Spawn("SoundSequenceSlot"));
				master.Sequence = master.StartSoundSequence (slot, 0);
			}
			master.Sequence.AddChoice (args[0], SeqNode.ENVIRONMENT);
			Destroy ();
		}
	}

	//==========================================================================
	//
	// ASoundSequence :: MarkPrecacheSounds
	//
	//==========================================================================

	override void MarkPrecacheSounds()
	{
		Super.MarkPrecacheSounds();
		SeqNode.MarkPrecacheSounds(args[0], SeqNode.ENVIRONMENT);
	}

	//==========================================================================
	//
	// ASoundSequence :: Activate
	//
	//==========================================================================

	override void Activate (Actor activator)
	{
		StartSoundSequenceID (args[0], SeqNode.ENVIRONMENT, args[1]);
	}

	//==========================================================================
	//
	// ASoundSequence :: Deactivate
	//
	//==========================================================================

	override void Deactivate (Actor activator)
	{
		StopSoundSequence ();
	}


}

// Heretic Sound sequences -----------------------------------------------------------

class HereticSoundSequence1 : SoundSequence
{
	default
	{
		Args 0;
	}
}

class HereticSoundSequence2 : SoundSequence
{
	default
	{
		Args 1;
	}
}

class HereticSoundSequence3 : SoundSequence
{
	default
	{
		Args 2;
	}
}

class HereticSoundSequence4 : SoundSequence
{
	default
	{
		Args 3;
	}
}

class HereticSoundSequence5 : SoundSequence
{
	default
	{
		Args 4;
	}
}

class HereticSoundSequence6 : SoundSequence
{
	default
	{
		Args 5;
	}
}

class HereticSoundSequence7 : SoundSequence
{
	default
	{
		Args 6;
	}
}

class HereticSoundSequence8 : SoundSequence
{
	default
	{
		Args 7;
	}
}

class HereticSoundSequence9 : SoundSequence
{
	default
	{
		Args 8;
	}
}

class HereticSoundSequence10 : SoundSequence
{
	default
	{
		Args 9;
	}
}
