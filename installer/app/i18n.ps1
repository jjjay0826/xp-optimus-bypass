if (-not (Get-Variable -Name Language -Scope Script -ErrorAction SilentlyContinue)) { $script:Language = 'en' }
$script:Translations = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'i18n.json') -Raw -Encoding UTF8 | ConvertFrom-Json
function T([string]$Text) {
    if ($script:Language -eq 'zh-TW') { return $Text }
    $property = $script:Translations.PSObject.Properties[$Text]
    if ($property) { return [string]$property.Value }
    return $Text
}
function Update-ControlLanguage($Control) {
    if ($Control -isnot [Windows.Forms.TextBox] -and $Control -isnot [Windows.Forms.ComboBox]) {
        foreach ($property in $script:Translations.PSObject.Properties) {
            if ($Control.Text -ceq $property.Name -or $Control.Text -ceq [string]$property.Value) {
                if ($script:Language -eq 'zh-TW') { $Control.Text = $property.Name }
                else { $Control.Text = [string]$property.Value }
                break
            }
        }
    }
    foreach ($child in $Control.Controls) { Update-ControlLanguage $child }
}
