
// To run: 
// dotnet tool install -g dotnet-script
// dotnet script GeneratePrompt.csx
// Change the top line to reference the NuGet package version instead
// "Usage:   dotnet script GeneratePrompt.csx --<OutputFile> "<TextPrompt>\"
// "Example: dotnet script GeneratePrompt.csx --asr_ready.wav "Please speak your command.";

#r "nuget: System.Speech, 8.0.0"

using System;
using System.IO;
using System.Speech.AudioFormat;
using System.Speech.Synthesis;

// -------------------------------------------------------------------------
// Command-line Argument Parsing
// -------------------------------------------------------------------------
if (Args.Count < 2)
{
    Console.WriteLine("Error: Missing parameters.");
    Console.WriteLine("Usage:   dotnet script GeneratePrompt.csx -- \"<OutputFile>\" \"<TextPrompt>\"");
    Console.WriteLine("Example: dotnet script GeneratePrompt.csx -- \"asr_ready.wav\" \"Please speak your command.\"");
    Environment.Exit(1);
}

string targetFileName = Args[0];
string textPrompt     = Args[1];

// Base path mapping pointing straight to your custom sound asset directory
string baseSoundsPath = @"C:\inetpub\SIPServer\sounds\custom\";
string outputPath     = Path.Combine(baseSoundsPath, targetFileName);

// Ensure the directory path exists before writing out the WAV file
Directory.CreateDirectory(Path.GetDirectoryName(outputPath));

Console.WriteLine($"Generating prompt file: {outputPath}");
Console.WriteLine($"Synthesizing text text: \"{textPrompt}\"");


// Speech Synthesis Engine

using (SpeechSynthesizer synth = new SpeechSynthesizer())
{
    // Explicitly target Microsoft Zira
    // Bulletproof matching: finds the voice even if it has "Desktop" in the name
var ziraVoice = synth.GetInstalledVoices()
    .FirstOrDefault(v => v.VoiceInfo.Name.Contains("Microsoft Zira"));

if (ziraVoice != null)
{
    synth.SelectVoice(ziraVoice.VoiceInfo.Name);
}
else
{
    Console.WriteLine("Warning: Microsoft Zira not found, using system default.");
}

    // Configure the output structure to exactly match broadcast telecom audio:
    // 48kHz sampling rate, 16-bit depth, Mono PCM configuration.
    SpeechAudioFormatInfo format = new SpeechAudioFormatInfo(
        48000, 
        AudioBitsPerSample.Sixteen, 
        AudioChannel.Mono
    );

    synth.SetOutputToWaveFile(outputPath, format);
    
    // Process the text to speech
    synth.Speak(textPrompt);
}

Console.WriteLine("Success: Voice prompt generated cleanly at 48kHz Mono.");