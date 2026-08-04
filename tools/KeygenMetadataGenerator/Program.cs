using System;
using System.Text.Json;
using System.Collections.Generic;

namespace KeygenMetadataGenerator
{
    class Program
    {
        static void Main(string[] args)
        {
            Console.WriteLine("==============================================");
            Console.WriteLine("  Keygen License Metadata Generator");
            Console.WriteLine("==============================================");
            Console.WriteLine();

            var metadata = new Dictionary<string, object>();

            // Collect domain
            Console.Write("Enter domain (e.g., *.brucepower.com or leave empty for no domain restriction): ");
            string domain = Console.ReadLine()?.Trim();
            if (!string.IsNullOrEmpty(domain))
            {
                metadata["domain"] = domain;
            }

            // Collect fingerprint
            Console.WriteLine();
            Console.Write("Enter machine fingerprint (SHA-256 hex string, or leave empty for no fingerprint restriction): ");
            string fingerprint = Console.ReadLine()?.Trim();
            if (!string.IsNullOrEmpty(fingerprint))
            {
                metadata["fingerprint"] = fingerprint;
            }

            // Inform about OR logic
            if (!string.IsNullOrEmpty(domain) && !string.IsNullOrEmpty(fingerprint))
            {
                Console.WriteLine();
                Console.WriteLine("NOTE: License will match if EITHER domain OR fingerprint matches (OR logic).");
            }

            // Collect modules
            Console.WriteLine();
            Console.WriteLine("Enter modules:");
            Console.WriteLine("  - Type 'all' for all modules");
            Console.WriteLine("  - Or enter comma/slash-separated list (e.g., mod_aes67/mod_opus/mod_conference)");
            Console.Write("Modules: ");
            string modules = Console.ReadLine()?.Trim();
            if (!string.IsNullOrEmpty(modules))
            {
                metadata["modules"] = modules;
            }

            // Collect max machines (optional)
            Console.WriteLine();
            Console.Write("Enter max machines (leave empty for unlimited): ");
            string maxMachinesInput = Console.ReadLine()?.Trim();
            if (!string.IsNullOrEmpty(maxMachinesInput) && int.TryParse(maxMachinesInput, out int maxMachines))
            {
                metadata["maxMachines"] = maxMachines;
            }

            // Collect expiry date (optional)
            Console.WriteLine();
            Console.Write("Enter expiry date (YYYY-MM-DD or leave empty for no expiry): ");
            string expiryInput = Console.ReadLine()?.Trim();
            if (!string.IsNullOrEmpty(expiryInput) && DateTime.TryParse(expiryInput, out DateTime expiry))
            {
                metadata["expiry"] = expiry.ToString("yyyy-MM-ddT00:00:00.000Z");
            }

            // Collect any additional custom fields
            Console.WriteLine();
            Console.Write("Add custom fields? (y/n): ");
            if (Console.ReadLine()?.Trim().ToLower() == "y")
            {
                while (true)
                {
                    Console.WriteLine();
                    Console.Write("Enter custom field name (or press Enter to finish): ");
                    string fieldName = Console.ReadLine()?.Trim();
                    if (string.IsNullOrEmpty(fieldName)) break;

                    Console.Write($"Enter value for '{fieldName}': ");
                    string fieldValue = Console.ReadLine()?.Trim();
                    if (!string.IsNullOrEmpty(fieldValue))
                    {
                        metadata[fieldName] = fieldValue;
                    }
                }
            }

            // Generate JSON
            Console.WriteLine();
            Console.WriteLine("==============================================");
            Console.WriteLine("Generated Metadata JSON:");
            Console.WriteLine("==============================================");

            var options = new JsonSerializerOptions 
            { 
                WriteIndented = true,
                Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
            };
            string json = JsonSerializer.Serialize(metadata, options);
            Console.WriteLine(json);

            Console.WriteLine();
            Console.WriteLine("==============================================");
            Console.WriteLine("Compact JSON (for Keygen):");
            Console.WriteLine("==============================================");
            string compactJson = JsonSerializer.Serialize(metadata);
            Console.WriteLine(compactJson);

            Console.WriteLine();
            Console.WriteLine("==============================================");
            Console.WriteLine("Instructions:");
            Console.WriteLine("==============================================");
            Console.WriteLine("1. Copy the compact JSON above");
            Console.WriteLine("2. Go to Keygen dashboard");
            Console.WriteLine("3. Create/edit a license");
            Console.WriteLine("4. For ONLINE keys: Add each field individually as metadata");
            Console.WriteLine("5. For OFFLINE keys: Generate signed license (metadata will be included in API response only)");
            Console.WriteLine();
            Console.WriteLine("Note: For offline licenses with domain/module restrictions,");
            Console.WriteLine("      consider using a separate .meta file alongside the .lic file.");
            Console.WriteLine();

            Console.WriteLine("Press any key to exit...");
            Console.ReadKey();
        }
    }
}
