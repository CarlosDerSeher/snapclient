/* Shared UI helpers for settings pages
 * Exposes helper functions on window.settingsUI
 */
(function () {
  'use strict';

  function formatValue(value, unit, decimals) {
    if (value === undefined || value === null) return '';
    if (decimals !== undefined) return Number(value).toFixed(decimals) + (unit || '');
    return String(value) + (unit || '');
  }

  function debounce(fn, wait) {
    let t = null;
    return function () {
      const args = arguments;
      clearTimeout(t);
      t = setTimeout(() => fn.apply(this, args), wait);
    };
  }

  // Render a parameter control (enum, range, float) -- returns HTMLElement
  function renderParameter(param, currentSettings, onChange) {
    const controlDiv = document.createElement('div');
    controlDiv.className = 'parameter-control';

    // Priority: param.current (from schema response) > currentSettings[param.key] > param.default
    const value = (param.current !== undefined) ? param.current : 
                  ((currentSettings && currentSettings[param.key] !== undefined) ? currentSettings[param.key] : param.default);

    // Check if value is null (coefficients not available before I2S clock)
    const isValueUnavailable = (value === null || value === undefined);

    if (param.type === 'enum') {
      const label = document.createElement('label');
      label.textContent = param.name;
      controlDiv.appendChild(label);

      const select = document.createElement('select');
      if (param.readonly) select.disabled = true;
      param.values.forEach(option => {
        const opt = document.createElement('option');
        opt.value = option.value;
        opt.textContent = option.name;
        if (value == option.value) opt.selected = true;
        select.appendChild(opt);
      });
      select.onchange = function () {
        if (onChange) onChange(param.key, parseInt(this.value));
      };
      controlDiv.appendChild(select);
      return controlDiv;
    }

    if (param.type === 'radio') {
      const label = document.createElement('label');
      label.className = 'radio-group-label';
      label.textContent = param.name;
      controlDiv.appendChild(label);

      const radioGroup = document.createElement('div');
      radioGroup.className = 'radio-group';
      param.values.forEach(option => {
        const radioLabel = document.createElement('label');
        radioLabel.className = 'radio-option';

        const radio = document.createElement('input');
        radio.type = 'radio';
        radio.name = param.key;
        radio.value = option.value;
        if (value == option.value) radio.checked = true;
        if (param.readonly) radio.disabled = true;
        radio.onchange = function () {
          if (onChange) onChange(param.key, parseInt(this.value));
        };

        const span = document.createElement('span');
        span.textContent = option.name;

        radioLabel.appendChild(radio);
        radioLabel.appendChild(span);
        radioGroup.appendChild(radioLabel);
      });
      controlDiv.appendChild(radioGroup);
      return controlDiv;
    }

    if (param.type === 'range') {
      const info = document.createElement('div');
      info.className = 'param-info';
      const nameSpan = document.createElement('span');
      nameSpan.className = 'param-name';
      nameSpan.textContent = param.name;
      const valueSpan = document.createElement('span');
      valueSpan.className = 'param-value';
      valueSpan.textContent = isValueUnavailable ? 'n/a' : formatValue(value, param.unit, param.decimals);
      info.appendChild(nameSpan);
      info.appendChild(valueSpan);
      controlDiv.appendChild(info);

      const slider = document.createElement('input');
      slider.type = 'range';
      slider.min = param.min;
      slider.max = param.max;
      if (param.step !== undefined) slider.step = param.step;
      slider.value = isValueUnavailable ? param.min : value;
      if (param.readonly || isValueUnavailable) slider.disabled = true;

      slider.oninput = function () {
        valueSpan.textContent = formatValue(this.value, param.unit, param.decimals);
      };

      slider.onchange = function () {
        if (onChange) onChange(param.key, parseFloat(this.value));
      };

      controlDiv.appendChild(slider);
      return controlDiv;
    }

    if (param.type === 'float') {
      const label = document.createElement('label');
      label.textContent = param.name;
      controlDiv.appendChild(label);

      const input = document.createElement('input');
      input.type = 'number';
      // Use param.step if provided, default to 0.000001 for 6 decimal places
      const step = param.step !== undefined ? param.step : 0.000001;
      input.step = step;
      // Calculate decimals needed from step value (5.27 format = up to 6 decimals for 0.000001)
      let decimals = 1;
      if (Number.isFinite(step)) {
        const stepStr = String(step);
        if (stepStr.indexOf('.') !== -1) {
          decimals = stepStr.split('.')[1].length;
        }
      }
      input.setAttribute('data-decimals', decimals);
      
      if (param.min !== undefined) input.min = param.min;
      if (param.max !== undefined) input.max = param.max;
      
      // Display 'n/a' if value is unavailable, otherwise format with proper decimals
      if (isValueUnavailable) {
        input.value = 'n/a';
        input.disabled = true;
      } else {
        input.value = Number(value).toFixed(decimals);
        if (param.readonly) input.disabled = true;
      }
      
      input.onchange = function () {
        if (onChange) onChange(param.key, parseFloat(this.value));
      };
      // Update display on input to show proper formatting
      input.oninput = function () {
        const dec = parseInt(this.getAttribute('data-decimals'));
        this.value = Number(this.value).toFixed(dec);
      };
      controlDiv.appendChild(input);
      return controlDiv;
    }

    // Unknown type fallback
    const p = document.createElement('p');
    p.textContent = `Unsupported parameter type: ${param.type}`;
    controlDiv.appendChild(p);
    return controlDiv;
  }

  // Vertical slider renderer for EQ band display
  function renderVerticalSlider(param, currentSettings, onChange) {
    const sliderDiv = document.createElement('div');
    sliderDiv.className = 'eq-band-slider';

    // Priority: param.current (from schema response) > currentSettings[param.key] > param.default
    const value = (param.current !== undefined) ? param.current :
                  ((currentSettings && currentSettings[param.key] !== undefined) ? currentSettings[param.key] : param.default);

    const valueDisplay = document.createElement('div');
    valueDisplay.className = 'value-display';
    valueDisplay.textContent = `${value}${param.unit || ''}`;
    sliderDiv.appendChild(valueDisplay);

    const slider = document.createElement('input');
    slider.type = 'range';
    slider.min = param.min;
    slider.max = param.max;
    slider.step = param.step || 1;
    slider.value = value;
    slider.oninput = function () {
      valueDisplay.textContent = `${this.value}${param.unit || ''}`;
    };
    slider.onchange = function () {
      if (onChange) onChange(param.key, parseInt(this.value));
    };
    sliderDiv.appendChild(slider);

    const label = document.createElement('label');
    label.textContent = param.label || param.name;
    sliderDiv.appendChild(label);

    return sliderDiv;
  }

  // Check if a parameter should be visible based on visibleWhen condition
  function isParameterVisible(param, currentSettings) {
    if (!param.visibleWhen) return true;
    for (const [key, expectedValue] of Object.entries(param.visibleWhen)) {
      const actualValue = currentSettings[key];
      if (actualValue !== expectedValue) return false;
    }
    return true;
  }

  // Update visibility of parameters with visibleWhen conditions
  function updateConditionalVisibility(container, currentSettings) {
    container.querySelectorAll('[data-visible-when]').forEach(el => {
      try {
        const condition = JSON.parse(el.getAttribute('data-visible-when'));
        let visible = true;
        for (const [key, expectedValue] of Object.entries(condition)) {
          if (currentSettings[key] !== expectedValue) {
            visible = false;
            break;
          }
        }
        el.style.display = visible ? '' : 'none';
      } catch (e) {
        console.error('Error parsing visibleWhen:', e);
      }
    });
  }

  // Render a PEQ subgroup (freq, gain, Q grouped together)
  function renderPeqSubgroup(subgroup, currentSettings, onChange) {
    const groupDiv = document.createElement('div');
    groupDiv.className = 'peq-subgroup';

    const header = document.createElement('div');
    header.className = 'peq-subgroup-header';
    header.textContent = subgroup.name;
    groupDiv.appendChild(header);

    const paramsRow = document.createElement('div');
    paramsRow.className = 'peq-subgroup-params';

    subgroup.parameters.forEach(param => {
      const controlDiv = document.createElement('div');
      controlDiv.className = 'peq-param';

      const label = document.createElement('label');
      label.textContent = param.name;
      controlDiv.appendChild(label);

      if (param.type === 'enum') {
        // Render enum as dropdown
        const select = document.createElement('select');
        param.values.forEach(option => {
          const opt = document.createElement('option');
          opt.value = option.value;
          opt.textContent = option.name;
          if (param.current == option.value) opt.selected = true;
          select.appendChild(opt);
        });
        select.onchange = function () {
          if (onChange) onChange(param.key, parseInt(this.value));
        };
        controlDiv.appendChild(select);
      } else {
        // Render as range slider (default)
        const input = document.createElement('input');
        input.type = 'range';
        input.min = param.min;
        input.max = param.max;
        input.step = param.step || 1;
        input.value = param.current !== undefined ? param.current : param.default || param.min;

        const valueSpan = document.createElement('span');
        valueSpan.className = 'peq-param-value';
        const decimals = param.decimals !== undefined ? param.decimals : 0;
        valueSpan.textContent = Number(input.value).toFixed(decimals) + (param.unit || '');

        input.oninput = function () {
          valueSpan.textContent = Number(this.value).toFixed(decimals) + (param.unit || '');
        };
        input.onchange = function () {
          if (onChange) onChange(param.key, parseFloat(this.value));
        };

        controlDiv.appendChild(input);
        controlDiv.appendChild(valueSpan);
      }
      paramsRow.appendChild(controlDiv);
    });

    groupDiv.appendChild(paramsRow);
    return groupDiv;
  }

  // Render a loudness zone subgroup (threshold, bass, treble grouped together)
  function renderLoudnessZone(subgroup, currentSettings, onChange) {
    const groupDiv = document.createElement('div');
    groupDiv.className = 'loudness-zone';

    const header = document.createElement('div');
    header.className = 'loudness-zone-header';
    header.textContent = subgroup.name;
    groupDiv.appendChild(header);

    const paramsRow = document.createElement('div');
    paramsRow.className = 'loudness-zone-params';

    subgroup.parameters.forEach(param => {
      const controlDiv = document.createElement('div');
      controlDiv.className = 'loudness-zone-param';

      const label = document.createElement('label');
      label.textContent = param.name;
      controlDiv.appendChild(label);

      const input = document.createElement('input');
      input.type = 'range';
      input.min = param.min;
      input.max = param.max;
      input.step = param.step || 1;
      input.value = param.current !== undefined ? param.current : param.default || param.min;

      const valueSpan = document.createElement('span');
      valueSpan.className = 'loudness-zone-value';
      const decimals = param.decimals !== undefined ? param.decimals : 0;
      valueSpan.textContent = Number(input.value).toFixed(decimals) + (param.unit || '');

      input.oninput = function () {
        valueSpan.textContent = Number(this.value).toFixed(decimals) + (param.unit || '');
      };
      input.onchange = function () {
        if (onChange) onChange(param.key, parseFloat(this.value));
      };

      controlDiv.appendChild(input);
      controlDiv.appendChild(valueSpan);
      paramsRow.appendChild(controlDiv);
    });

    groupDiv.appendChild(paramsRow);
    return groupDiv;
  }

  // Render a section within a group (for biamp Low/High columns)
  function renderSection(section, currentSettings, onChange) {
    const sectionDiv = document.createElement('div');
    sectionDiv.className = 'biamp-section';
    if (section.layout) {
      sectionDiv.classList.add('section-' + section.layout);
    }

    if (section.name) {
      const title = document.createElement('h3');
      title.className = 'section-title';
      title.textContent = section.name;
      sectionDiv.appendChild(title);
    }

    const paramsContainer = document.createElement('div');
    paramsContainer.className = 'section-params';

    if (section.layout === 'vertical-sliders') {
      paramsContainer.classList.add('vertical-sliders-container');
      section.parameters.forEach(param => {
        // Use vertical slider only for range type, otherwise use standard renderer
        const controlDiv = (param.type === 'range')
          ? renderVerticalSlider(param, currentSettings, onChange)
          : renderParameter(param, currentSettings, onChange);
        if (param.visibleWhen) {
          controlDiv.setAttribute('data-visible-when', JSON.stringify(param.visibleWhen));
          if (!isParameterVisible(param, currentSettings)) {
            controlDiv.style.display = 'none';
          }
        }
        paramsContainer.appendChild(controlDiv);
      });
    } else if (section.layout === 'output-channel') {
      // Output channel layout: regular params + PEQ subgroups + generic subgroups + phase radio
      section.parameters.forEach(param => {
        let controlDiv;
        if (param.type === 'peq-subgroup' || param.type === 'subgroup') {
          controlDiv = renderPeqSubgroup(param, currentSettings, onChange);
        } else {
          controlDiv = renderParameter(param, currentSettings, onChange);
        }
        if (param.visibleWhen) {
          controlDiv.setAttribute('data-visible-when', JSON.stringify(param.visibleWhen));
          if (!isParameterVisible(param, currentSettings)) {
            controlDiv.style.display = 'none';
          }
        }
        paramsContainer.appendChild(controlDiv);
      });
    } else if (section.layout === 'loudness') {
      // Loudness layout: enable toggle + loudness-zone subgroups
      section.parameters.forEach(param => {
        let controlDiv;
        if (param.type === 'loudness-zone') {
          controlDiv = renderLoudnessZone(param, currentSettings, onChange);
        } else {
          controlDiv = renderParameter(param, currentSettings, onChange);
        }
        if (param.visibleWhen) {
          controlDiv.setAttribute('data-visible-when', JSON.stringify(param.visibleWhen));
          if (!isParameterVisible(param, currentSettings)) {
            controlDiv.style.display = 'none';
          }
        }
        paramsContainer.appendChild(controlDiv);
      });
    } else {
      section.parameters.forEach(param => {
        const controlDiv = renderParameter(param, currentSettings, onChange);
        if (param.visibleWhen) {
          controlDiv.setAttribute('data-visible-when', JSON.stringify(param.visibleWhen));
          if (!isParameterVisible(param, currentSettings)) {
            controlDiv.style.display = 'none';
          }
        }
        paramsContainer.appendChild(controlDiv);
      });
    }

    sectionDiv.appendChild(paramsContainer);
    return sectionDiv;
  }

  // Visibility helpers for EQ groups
  function updateGroupVisibility(groupDiv, currentSettings) {
    const layout = groupDiv.getAttribute('data-layout');
    const channel = groupDiv.getAttribute('data-channel');
    if (!layout) return;
    const uiMode = (currentSettings && currentSettings.eq_ui_mode !== undefined) ? currentSettings.eq_ui_mode : 0;

    if (layout === 'eq-bands') {
      if (uiMode === 1) {
        groupDiv.style.display = (channel === 'left') ? 'block' : 'none';
      } else if (uiMode === 2) {
        groupDiv.style.display = 'block';
      } else {
        groupDiv.style.display = 'none';
      }
    }

    if (layout === 'biquad-manual') {
      // Always show biquad section, but expand/collapse based on mode
      groupDiv.style.display = 'block';
      const isManualMode = (uiMode === 4);

      if (isManualMode) {
        groupDiv.classList.remove('collapsed');
      } else {
        groupDiv.classList.add('collapsed');
      }

      // Update readonly state of inputs when mode changes
      const inputs = groupDiv.querySelectorAll('input[type="number"]');
      inputs.forEach(input => {
        const isA0 = input.id && input.id.includes('_a0');
        if (isA0) {
          input.disabled = true; // a0 always readonly
        } else {
          input.disabled = !isManualMode;
        }
      });

      // Update button group: always visible, but Apply button only enabled in manual mode
      const buttonGroup = groupDiv.querySelector('.button-group');
      if (buttonGroup) {
        buttonGroup.style.display = 'flex'; // Always visible
        const applyButton = buttonGroup.querySelector('.apply-button');
        if (applyButton) {
          applyButton.disabled = !isManualMode;
        }
      }
    }

    if (layout === 'eq-presets') {
      groupDiv.style.display = (uiMode === 3) ? 'block' : 'none';
    }

    if (layout === 'biamp-crossover') {
      groupDiv.style.display = (uiMode === 4) ? 'block' : 'none';
    }

    // Update conditional visibility within the group
    updateConditionalVisibility(groupDiv, currentSettings);
  }

  function updateAllGroupVisibility(currentSettings) {
    document.querySelectorAll('.parameter-group[data-layout]').forEach(group => {
      updateGroupVisibility(group, currentSettings);
    });
  }

  // Export API
  window.settingsUI = {
    formatValue,
    debounce,
    renderParameter,
    renderVerticalSlider,
    renderPeqSubgroup,
    renderLoudnessZone,
    renderSection,
    isParameterVisible,
    updateConditionalVisibility,
    updateGroupVisibility,
    updateAllGroupVisibility,
  };

})();
